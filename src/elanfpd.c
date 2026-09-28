#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include "device.h"
#include "storage.h"

#define FPRINT_INTERFACE_MANAGER "net.reactivated.Fprint.Manager"
#define FPRINT_INTERFACE_DEVICE  "net.reactivated.Fprint.Device"
#define FPRINT_DEVICE_PATH       "/net/reactivated/Fprint/Device/0"

sd_bus *g_bus = NULL;

static void on_enroll_status(const char *result, bool done, void *userdata) {
    (void)userdata;
    sd_bus_emit_signal(g_bus, FPRINT_DEVICE_PATH, FPRINT_INTERFACE_DEVICE,
                       "EnrollStatus", "sb", result, done ? 1 : 0);
}

static void on_verify_status(const char *result, bool done, void *userdata) {
    (void)userdata;
    sd_bus_emit_signal(g_bus, FPRINT_DEVICE_PATH, FPRINT_INTERFACE_DEVICE,
                       "VerifyStatus", "sb", result, done ? 1 : 0);
}

static int method_get_devices(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    sd_bus_message *reply = NULL;
    if (sd_bus_message_new_method_return(m, &reply) < 0) return -1;
    sd_bus_message_open_container(reply, 'a', "o");
    sd_bus_message_append(reply, "o", FPRINT_DEVICE_PATH);
    sd_bus_message_close_container(reply);
    int r = sd_bus_send(g_bus, reply, NULL);
    sd_bus_message_unref(reply);
    return (r >= 0) ? 1 : r;
}

static int method_get_default_device(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    return sd_bus_reply_method_return(m, "o", FPRINT_DEVICE_PATH);
}

static const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetDevices", "", "ao", method_get_devices, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDefaultDevice", "", "o", method_get_default_device, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static void resolve_username(sd_bus_message *m, const char *username, char *out, size_t sz) {
    out[0] = '\0';
    if (username && username[0] != '\0') {
        snprintf(out, sz, "%s", username);
        return;
    }

    // Check message credentials
    sd_bus_creds *creds = (sd_bus_creds *)sd_bus_message_get_creds(m);
    uid_t caller_uid;
    if (creds && sd_bus_creds_get_uid(creds, &caller_uid) >= 0) {
        struct passwd *pw = getpwuid(caller_uid);
        if (pw && pw->pw_name) {
            snprintf(out, sz, "%s", pw->pw_name);
            return;
        }
    }

    // Check enrolled user directory if only one profile exists
    DIR *d = opendir(STORAGE_BASE_DIR);
    if (d) {
        struct dirent *de;
        int count = 0;
        char found[256] = {0};
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char subpath[512];
            snprintf(subpath, sizeof(subpath), "%s/%s", STORAGE_BASE_DIR, de->d_name);
            struct stat st;
            if (stat(subpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(found, sizeof(found), "%s", de->d_name);
                count++;
            }
        }
        closedir(d);
        if (count == 1) {
            snprintf(out, sz, "%.*s", (int)(sz - 1), found);
            return;
        }
    }

    // Environment fallback
    const char *env_user = getenv("SUDO_USER");
    if (!env_user) env_user = getenv("USER");
    if (env_user && env_user[0]) {
        snprintf(out, sz, "%s", env_user);
    }
}

static int method_claim(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *username = NULL;
    if (sd_bus_message_read(m, "s", &username) < 0) return -1;

    char resolved_user[128] = {0};
    resolve_username(m, username, resolved_user, sizeof(resolved_user));

    if (resolved_user[0] == '\0') {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.InternalError", "Could not resolve user");
    }

    const char *sender = sd_bus_message_get_sender(m);
    if (device_claim(sender, resolved_user) < 0) {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.DeviceBusy", "Device is claimed");
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_release(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *sender = sd_bus_message_get_sender(m);
    if (device_release(sender) < 0) {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.PermissionDenied", "Not claimed by caller");
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_enroll_start(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *finger = NULL;
    if (sd_bus_message_read(m, "s", &finger) < 0) return -1;

    if (device_start_enroll(finger, on_enroll_status, NULL) < 0) {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.DeviceBusy", "Device busy");
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_enroll_stop(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    device_stop_enroll();
    return sd_bus_reply_method_return(m, "");
}

static int method_verify_start(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *finger = NULL;
    if (sd_bus_message_read(m, "s", &finger) < 0) return -1;

    if (device_start_verify(finger, on_verify_status, NULL) < 0) {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.DeviceBusy", "Device busy");
    }

    const char *selected = (finger && strcmp(finger, "any") != 0) ? finger : "right-index-finger";
    sd_bus_emit_signal(g_bus, FPRINT_DEVICE_PATH, FPRINT_INTERFACE_DEVICE,
                       "VerifyFingerSelected", "s", selected);

    return sd_bus_reply_method_return(m, "");
}

static int method_verify_stop(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    device_stop_verify();
    return sd_bus_reply_method_return(m, "");
}

static int method_list_enrolled(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    const char *username = NULL;
    if (sd_bus_message_read(m, "s", &username) < 0) return -1;

    char resolved_user[128] = {0};
    resolve_username(m, username, resolved_user, sizeof(resolved_user));

    char **fingers = NULL;
    size_t count = 0;
    storage_list_fingers(resolved_user, &fingers, &count);

    sd_bus_message *reply = NULL;
    if (sd_bus_message_new_method_return(m, &reply) < 0) {
        storage_free_list(fingers, count);
        return -1;
    }

    sd_bus_message_open_container(reply, 'a', "s");
    for (size_t i = 0; i < count; i++) sd_bus_message_append(reply, "s", fingers[i]);
    sd_bus_message_close_container(reply);

    storage_free_list(fingers, count);
    int r = sd_bus_send(g_bus, reply, NULL);
    sd_bus_message_unref(reply);
    return (r >= 0) ? 1 : r;
}

static int method_delete_enrolled(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    const char *username = NULL;
    if (sd_bus_message_read(m, "s", &username) < 0) return -1;

    char resolved_user[128] = {0};
    resolve_username(m, username, resolved_user, sizeof(resolved_user));

    storage_delete_user(resolved_user);
    return sd_bus_reply_method_return(m, "");
}

static int method_delete_enrolled_finger(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *finger = NULL;
    if (sd_bus_message_read(m, "s", &finger) < 0) return -1;

    const char *target_user = device_get_claimed_user();
    if (!target_user || target_user[0] == '\0') {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.ClaimDevice", "Device not claimed");
    }

    if (storage_delete_finger(target_user, finger) < 0) {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.PrintsNotDeleted", "Finger not found or could not be deleted");
    }
    return sd_bus_reply_method_return(m, "");
}

static int method_delete_enrolled_fingers2(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    const char *target_user = device_get_claimed_user();
    if (!target_user || target_user[0] == '\0') {
        return sd_bus_error_setf(ret_error, "net.reactivated.Fprint.Error.ClaimDevice", "Device not claimed");
    }

    storage_delete_user(target_user);
    return sd_bus_reply_method_return(m, "");
}

static int prop_get_name(sd_bus *bus, const char *path, const char *interface,
                         const char *property, sd_bus_message *reply, void *userdata,
                         sd_bus_error *ret_error) {
    (void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
    return sd_bus_message_append(reply, "s", "ELAN 04f3:0903 Matcher");
}

static const sd_bus_vtable device_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("name", "s", prop_get_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Claim", "s", "", method_claim, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", method_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EnrollStart", "s", "", method_enroll_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EnrollStop", "", "", method_enroll_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("VerifyStart", "s", "", method_verify_start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("VerifyStop", "", "", method_verify_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListEnrolledFingers", "s", "as", method_list_enrolled, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DeleteEnrolledFingers", "s", "", method_delete_enrolled, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DeleteEnrolledFingers2", "", "", method_delete_enrolled_fingers2, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DeleteEnrolledFinger", "s", "", method_delete_enrolled_finger, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("EnrollStatus", "sb", 0),
    SD_BUS_SIGNAL("VerifyStatus", "sb", 0),
    SD_BUS_SIGNAL("VerifyFingerSelected", "s", 0),
    SD_BUS_VTABLE_END
};

int main(int argc, char *argv[]) {
    setlinebuf(stdout);
    const char *model = (argc > 1) ? argv[1] : "/usr/share/elanfp/model.onnx";
    if (device_init(model) < 0) {
        fprintf(stderr, "Failed to init device and ONNX matcher.\n");
        return 1;
    }

    int r_bus = sd_bus_new(&g_bus);
    if (r_bus >= 0) {
        sd_bus_set_address(g_bus, "unix:path=/run/dbus/system_bus_socket");
        sd_bus_set_bus_client(g_bus, 1);
        sd_bus_negotiate_creds(g_bus, 1, SD_BUS_CREDS_UID | SD_BUS_CREDS_EUID);
        r_bus = sd_bus_start(g_bus);
    }
    if (r_bus < 0) {
        if (sd_bus_open_system(&g_bus) < 0) {
            fprintf(stderr, "Failed to connect to system bus.\n");
            return 1;
        }
    }

    int r = sd_bus_add_object_vtable(g_bus, NULL, "/net/reactivated/Fprint/Manager",
                                     FPRINT_INTERFACE_MANAGER, manager_vtable, NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to add Manager vtable: %s\n", strerror(-r));
        return 1;
    }

    r = sd_bus_add_object_vtable(g_bus, NULL, FPRINT_DEVICE_PATH,
                                 FPRINT_INTERFACE_DEVICE, device_vtable, NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to add Device vtable: %s\n", strerror(-r));
        return 1;
    }

    int r_name = sd_bus_request_name(g_bus, "net.reactivated.Fprint", SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT);
    if (r_name < 0) {
        fprintf(stderr, "Failed to acquire service name net.reactivated.Fprint: %s\n", strerror(-r_name));
        return 1;
    }

    printf("elanfpd running on net.reactivated.Fprint\n");

    while (1) {
        int r_proc = sd_bus_process(g_bus, NULL);
        if (r_proc < 0) break;
        if (r_proc > 0) continue;
        r_proc = sd_bus_wait(g_bus, (uint64_t)-1);
        if (r_proc < 0) break;
    }

    device_cleanup();
    sd_bus_unref(g_bus);
    return 0;
}