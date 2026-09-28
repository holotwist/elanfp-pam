[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg?style=for-the-badge)](./LICENSE)

# elanfp-pam

Linux PAM module and user enrollment/verification tools for ELAN (04f3:0903) capacitive fingerprint sensors using a SCNN as matcher

## Dependencies

- C11 compiler (GCC or Clang)
- CMake 3.16+
- `libusb-1.0`
- `pam` development headers
- `onnxruntime` C API libraries and headers

### Package Installation

Debian / Ubuntu / Linux Mint:
```bash
sudo apt update
sudo apt install build-essential cmake libusb-1.0-0-dev libpam0g-dev
```

Arch Linux / Manjaro:
```bash
sudo pacman -S base-devel cmake libusb pam
```

Fedora / RHEL:
```bash
sudo dnf install gcc cmake libusb1-devel pam-devel
```

Note: Ensure ONNX Runtime C headers and libraries are installed system-wide or located under `./onnx/include` and `./onnx/lib`.

---

## Compilation & Installation

### Using `build.sh`

```bash
# Build binaries and PAM module in Release mode
./build.sh

# Build targeting modern x86-64 (x86-64-v3)
./build.sh -m

# Build and install to system paths (root)
sudo ./build.sh -i

# Clean wipe and rebuild
./build.sh -c -r
```

### Using CMake Directly

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

The installer places:
- Binaries (`elanfp-enroll`, `elanfp-verify`) into `/usr/bin/`
- PAM module (`pam_elanfp.so`) into `/usr/lib/security/`
- Model file (`model.onnx` / `model.onnx.data`) into `/usr/share/elanfp/`
- Templates directory at `/var/lib/elanfp/templates/`

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

Run enrollment for a target user (requires 8 touches)

```bash
sudo elanfp-enroll <username>
```

Templates are saved to `/var/lib/elanfp/templates/<username>.dat`.

### Test Verification

Test matching directly via CLI without going through PAM:

```bash
elanfp-verify <username>
```

### Configure PAM

Add `pam_elanfp.so` to your target PAM service (e.g. `/etc/pam.d/sudo` or `/etc/pam.d/system-auth`):

```pam
auth        sufficient    pam_elanfp.so timeout=4
```

The module allows up to 2 attempts.

---

## License

Licensed under GPLv3. See [LICENSE](./LICENSE) for details.