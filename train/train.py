#!/usr/bin/env python3
import os
import argparse
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Subset
from torchvision import datasets, transforms
from tqdm import tqdm

from model import TinyNet, ArcMarginProduct


def evaluate_verification(model, val_loader, device):
    # Evaluates True Acceptance vs False Acceptance on unseen identities
    # Computes genuine pair cosine similarities and imposter pair cosine similarities
    model.eval()
    embeddings_list = []
    labels_list = []

    with torch.no_grad():
        for imgs, labels in val_loader:
            imgs = imgs.to(device)
            feats = model(imgs)
            embeddings_list.append(feats.cpu())
            labels_list.append(labels)

    embeddings = torch.cat(embeddings_list, dim=0).numpy()
    labels = torch.cat(labels_list, dim=0).numpy()

    # Sample random pairs for fast evaluation
    n = len(labels)
    genuine_scores = []
    imposter_scores = []

    # Compute dot products between sample pairs
    for i in range(min(n, 400)):
        for j in range(i + 1, min(n, 400)):
            sim = float(np.dot(embeddings[i], embeddings[j]))
            if labels[i] == labels[j]:
                genuine_scores.append(sim)
            else:
                if len(imposter_scores) < 2000:
                    imposter_scores.append(sim)

    gen_mean = np.mean(genuine_scores) if genuine_scores else 0.0
    imp_mean = np.mean(imposter_scores) if imposter_scores else 0.0

    # TAR at threshold 0.65
    tar = np.mean(np.array(genuine_scores) >= 0.65) if genuine_scores else 0.0
    far = np.mean(np.array(imposter_scores) >= 0.65) if imposter_scores else 0.0

    return gen_mean, imp_mean, tar, far


def export_onnx(model, save_path="model.onnx", device="cpu", single_file=True):
    model.eval()
    dummy_input = torch.randn(1, 1, 79, 79, device=device)
    torch.onnx.export(
        model,
        dummy_input,
        save_path,
        export_params=True,
        opset_version=18,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["embedding"],
        dynamic_axes={"input": {0: "batch_size"}, "embedding": {0: "batch_size"}},
    )

    if single_file:
        try:
            import onnx
            from onnx.external_data_helper import convert_model_from_external_data

            onnx_model = onnx.load(save_path)
            convert_model_from_external_data(onnx_model)
            onnx.save_model(onnx_model, save_path, save_as_external_data=False)

            # Remove external data file if one was generated
            for extra in [save_path + ".data", os.path.splitext(save_path)[0] + ".data"]:
                if os.path.exists(extra):
                    os.remove(extra)
        except ImportError:
            print("Warning: Install 'onnx' (pip install onnx) to consolidate weights into one file.")

    print(f"Exported model to: {save_path}")


def main():
    parser = argparse.ArgumentParser(description="Trainer")
    parser.add_argument("--data_dir", type=str, default="dataset_79x79")
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch_size", type=int, default=64)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--embedding_dim", type=int, default=128)
    parser.add_argument("--val_split", type=float, default=0.10, help="Fraction of fingers reserved for validation")
    parser.add_argument("--export_only", type=str, nargs="?", const="best_model.pth", default=None,
                        help="Skip training, load existing checkpoint, and export ONNX")
    parser.add_argument("--single_file", action="store_true", default=True,
                        help="Consolidate all weights inside a single .onnx file (default: True)")
    args = parser.parse_args()

    device = torch.device("cpu")
    torch.set_num_threads(6)

    # Export existing checkpoint directly without loading dataset
    if args.export_only:
        ckpt_path = args.export_only
        print(f"\nLoading checkpoint from: {ckpt_path}")
        if not os.path.exists(ckpt_path):
            print(f"Error: Checkpoint file not found: {ckpt_path}")
            return
        model = TinyNet(embedding_dim=args.embedding_dim).to(device)
        model.load_state_dict(torch.load(ckpt_path, map_location="cpu"))
        export_onnx(model, save_path="model.onnx", device="cpu", single_file=args.single_file)
        print("Export completed")
        return

    print(f"Using device: {device} (OMP threads: {torch.get_num_threads()})")

    # Dataset transforms
    # Images are 79x79 grayscale. Map [0, 255] to [-1.0, 1.0]
    transform = transforms.Compose([
        transforms.Grayscale(num_output_channels=1),
        transforms.ToTensor(),
        transforms.Normalize(mean=[0.5], std=[0.5]),
    ])

    full_dataset = datasets.ImageFolder(root=args.data_dir, transform=transform)
    num_classes = len(full_dataset.classes)
    print(f"Found {len(full_dataset)} images across {num_classes} finger classes.")

    # Split dataset by identities
    num_val_classes = max(1, int(num_classes * args.val_split))
    num_train_classes = num_classes - num_val_classes

    train_indices = [i for i, (_, label) in enumerate(full_dataset.samples) if label < num_train_classes]
    val_indices = [i for i, (_, label) in enumerate(full_dataset.samples) if label >= num_train_classes]

    train_dataset = Subset(full_dataset, train_indices)
    val_dataset = Subset(full_dataset, val_indices)

    print(f"Training on {num_train_classes} classes ({len(train_dataset)} images)")
    print(f"Validating on {num_val_classes} unseen classes ({len(val_dataset)} images)")

    # 2 background workers for disk loading, 4-6 cores for backprop
    train_loader = DataLoader(train_dataset, batch_size=args.batch_size, shuffle=True, num_workers=2, pin_memory=False)
    val_loader = DataLoader(val_dataset, batch_size=args.batch_size, shuffle=False, num_workers=2, pin_memory=False)

    # Instantiate model and arcface head
    model = TinyNet(embedding_dim=args.embedding_dim).to(device)
    arcface = ArcMarginProduct(in_features=args.embedding_dim, out_features=num_train_classes, s=32.0, m=0.35).to(device)

    criterion = nn.CrossEntropyLoss()
    optimizer = torch.optim.AdamW(
        list(model.parameters()) + list(arcface.parameters()),
        lr=args.lr,
        weight_decay=1e-4
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs, eta_min=1e-5)

    best_tar = 0.0

    print("\nStarting Training...")
    for epoch in range(1, args.epochs + 1):
        # Margin warm-up schedule
        # 0.0 for epochs 1-3, linearly ramp to 0.35 by epoch 8
        if epoch <= 3:
            current_m = 0.0
        elif epoch <= 8:
            current_m = 0.35 * (epoch - 3) / 5.0
        else:
            current_m = 0.35

        model.train()
        arcface.train()
        running_loss = 0.0
        correct = 0
        total = 0

        pbar = tqdm(train_loader, desc=f"Epoch {epoch:02d}/{args.epochs:02d} (m={current_m:.2f})")
        for imgs, labels in pbar:
            imgs, labels = imgs.to(device), labels.to(device)

            optimizer.zero_grad()
            embeddings = model(imgs)
            logits, raw_cosines = arcface(embeddings, labels, current_m=current_m)
            loss = criterion(logits, labels)

            loss.backward()
            optimizer.step()

            running_loss += loss.item() * imgs.size(0)
            # Evaluate classification accuracy from unpenalized cosines
            preds = raw_cosines.argmax(dim=1)
            correct += (preds == labels).sum().item()
            total += labels.size(0)

            pbar.set_postfix({"loss": f"{loss.item():.3f}", "acc": f"{correct/total*100:.1f}%"})

        scheduler.step()
        epoch_loss = running_loss / total
        epoch_acc = (correct / total) * 100.0

        # Evaluate on unseen validation fingers
        gen_mean, imp_mean, tar, far = evaluate_verification(model, val_loader, device)

        print(f"[Epoch {epoch:02d}] Loss: {epoch_loss:.4f} | Train Acc: {epoch_acc:.1f}% | "
              f"Genuine Cosine: {gen_mean:.3f} | Imposter Cosine: {imp_mean:.3f} | "
              f"TAR@0.65: {tar*100:.1f}% | FAR@0.65: {far*100:.2f}%")

        # Evaluate model quality by the separation gap (gen, imp)
        sep_gap = gen_mean - imp_mean

        if epoch >= 8 and sep_gap >= best_tar:
            best_tar = sep_gap
            torch.save(model.state_dict(), "best_model.pth")
            print(f"  -> Best discriminative model saved (Gap: {sep_gap:.3f}, Gen: {gen_mean:.3f}, Imp: {imp_mean:.3f})")

        # Always save the latest epoch weights as fallback
        torch.save(model.state_dict(), "latest_model.pth")

    # Export final ONNX model from the best discriminative checkpoint
    print("\nExporting model to ONNX")
    export_path = "best_model.pth" if os.path.exists("best_model.pth") else "latest_model.pth"
    print(f"Loading weights from {export_path}")
    model.load_state_dict(torch.load(export_path, map_location="cpu"))
    export_onnx(model.to("cpu"), save_path="model.onnx", device="cpu", single_file=args.single_file)
    print("Done")


if __name__ == "__main__":
    main()