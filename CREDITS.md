# Credits & Third-Party Licenses

`elanfp-pam` relies on and adapts open-source research, drivers, libraries, and datasets.

---

## Hardware Driver & Protocol Attribution

### libfprint (ELAN Driver)
* **Resource:** Hardware command sequences, USB endpoint mappings, sensor dimension queries, and polling protocol for the ELAN 04f3:0903 sensor.
* **Source:** [`libfprint/drivers/elan.c`](https://gitlab.freedesktop.org/libfprint/libfprint/-/blob/master/libfprint/drivers/elan.c)
* **Authors:** Igor Filatov, Vasily Khoruzhick, and the `libfprint` contributors.
* **Website:** [https://gitlab.freedesktop.org/libfprint/libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
* **License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1-or-later).

---

## ML and Dataset Attribution

### Anguli: Synthetic fingerprint impressions (10K Sample)
* **Resource:** Training dataset.
* **Authors:** Anguli fingerprint generator project.
* **Website:** [https://dsl.cds.iisc.ac.in/projects/Anguli/](https://dsl.cds.iisc.ac.in/projects/Anguli/)

---

### ArcFace (Additive angular margin loss)
* **Authors:** Jiankang Deng, Jia Guo, Niannan Xue, Stefanos Zafeiriou (InsightFace team).
* **Paper:** *ArcFace: Additive Angular Margin Loss for Deep Face Recognition* (CVPR 2019).
* **Website:** [https://github.com/deepinsight/insightface](https://github.com/deepinsight/insightface)
* **License:** MIT License.

---

## Runtime and System Dependencies

The C executables and PAM module link against the following libraries:

* **ONNX Runtime:** MIT License ([https://onnxruntime.ai/](https://onnxruntime.ai/))
* **libusb-1.0:** GNU LGPL v2.1 ([https://libusb.info/](https://libusb.info/))
* **Linux-PAM:** BSD-3-Clause / GPLv2 ([https://github.com/linux-pam/linux-pam](https://github.com/linux-pam/linux-pam))

---

## Training Dependencies

The offline dataset generator and model training scripts use:

* **PyTorch:** Modified BSD License ([https://pytorch.org/](https://pytorch.org/))
* **Torchvision:** BSD 3-Clause License ([https://github.com/pytorch/vision](https://github.com/pytorch/vision))
* **SciPy:** BSD 3-Clause License ([https://scipy.org/](https://scipy.org/))
* **Pillow:** HPND License ([https://python-pillow.org/](https://python-pillow.org/))
* **ONNX:** Apache 2.0 License ([https://onnx.ai/](https://onnx.ai/))
* **tqdm:** MIT / MPL 2.0 License ([https://github.com/tqdm/tqdm](https://github.com/tqdm/tqdm))

---

## Project License

`elanfp-pam` is distributed under the **GNU General Public License v3.0 (GPLv3)**.  
See the [LICENSE](LICENSE) file for full license text.