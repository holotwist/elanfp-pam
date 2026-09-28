[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg?style=for-the-badge)](./LICENSE)

# elanfpd

fprintd replacement daemon for ELAN (04f3:0903) capacitive fingerprint sensors using SCNN embeddings as matcher. Integrates with fprintd-enroll, standard pam_fprintd, and GNOME/KDE desktop settings.

## Dependencies

- C11 compiler (GCC or Clang)
- CMake 3.16+
- `libusb-1.0`
- `libsystemd` (sd-bus)
- `fprintd` and `pam_fprintd` system packages
- `onnxruntime` C API libraries and headers

### Package Installation

Debian / Ubuntu / Linux Mint:
```bash
sudo apt update
sudo apt install build-essential cmake libusb-1.0-0-dev libsystemd-dev fprintd libpam-fprintd
```

Arch Linux / Manjaro:
```bash
sudo pacman -S base-devel cmake libusb systemd-libs fprintd
```

Fedora / RHEL:
```bash
sudo dnf install gcc cmake libusb1-devel systemd-devel fprintd fprintd-pam
```

Note: Ensure ONNX Runtime C headers and libraries are installed system-wide or located under `./onnx/include` and `./onnx/lib`.

---

## Compilation & Installation

### Build and Install

```bash
# Clean build and install daemon
sudo ./build.sh -c -r -i
```

The installer places:
- Daemon binary (`elanfpd`) into `/usr/bin/`
- Systemd service (`elanfpd.service`) into `/etc/systemd/system/`
- D-Bus policy (`net.reactivated.Fprint.conf`) into `/usr/share/dbus-1/system.d/`
- Model file (`model.onnx`) into `/usr/share/elanfp/`
- Template vault at `/var/lib/fprint/`

### Configure & Enable Service

Enable and start `elanfpd` (it replaces `fprintd.service` automatically):

```bash
# Reload systemd definitions
sudo systemctl daemon-reload

# Enable and start elanfpd
sudo systemctl enable --now elanfpd.service
```

Verify the daemon is running and bound to the system D-Bus:

```bash
systemctl status elanfpd.service
```

---

## Training (Optional)

The repository uses a convolutional embedding model trained to project 79×79 capacitive patches onto a 128-D sphere

### Python Dependencies

```bash
pip install torch torchvision scipy pillow tqdm onnx
```

### Synthesize Dataset

Generate capacitive-style impressions. The model was trained using the **10K sample fingerprint impressions from [Anguli](https://dsl.cds.iisc.ac.in/projects/Anguli/)** as the source master dataset

```bash
python3 train/generate_dataset.py --input_dir /path/to/Anguli_10K --output_dir dataset_79x79 --num_fingers 1000 --impressions 12
```

### Train and Export ONNX

Train the network and export `model.onnx`:

```bash
python3 train/train.py --data_dir dataset_79x79 --epochs 30 --single_file
```

To re-export an existing checkpoint without retraining:

```bash
python3 train/train.py --export_only best_model.pth --single_file
```

---

## Usage

### Enroll a Fingerprint

Enroll (defaults to `right-index-finger`, requires 8 touches):

```bash
fprintd-enroll
```

Enroll a specific finger:

```bash
fprintd-enroll -f left-index-finger
```

### Verify Fingerprint

Test verification directly via CLI:

```bash
fprintd-verify
```

### Manage Enrolled Fingers

```bash
# List enrolled fingers for current user
fprintd-list "$USER"

# Delete all enrolled fingers for current user
fprintd-delete "$USER"
```

### Configure PAM

Enable fingerprint authentication using your distribution's PAM tooling:

**Ubuntu / Debian / Linux Mint:**
```bash
sudo pam-auth-update
# Ensure [*] Fingerprint authentication is checked
```

**Fedora / RHEL:**
```bash
sudo authselect enable-feature with-fingerprint
sudo authselect apply-changes
```

**Arch Linux:**
Add `pam_fprintd.so` as `sufficient` to `/etc/pam.d/system-auth`:
```pam
auth        sufficient    pam_fprintd.so
```

---

## License

Licensed under GPLv3. See [LICENSE](./LICENSE) for details.