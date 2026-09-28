#!/usr/bin/env python3

import os
import glob
import random
import argparse
import numpy as np
from PIL import Image
from scipy.ndimage import (
    gaussian_filter,
    map_coordinates,
    rotate,
    maximum_filter,
)
from tqdm import tqdm


def find_active_bbox(img_np, threshold=245):
    # Find active ridge area in Anguli print, ignoring white background.
    mask = img_np < threshold
    if not np.any(mask):
        return 0, 0, img_np.shape[1], img_np.shape[0]
    y_indices, x_indices = np.where(mask)
    return np.min(x_indices), np.min(y_indices), np.max(x_indices), np.max(y_indices)


def get_finger_anchor(img_np, patch_size=79):
    # Select a consistent anchor center (core area) for this finger.
    x_min, y_min, x_max, y_max = find_active_bbox(img_np)
    # Target the upper-middle active ridge area
    cx = (x_min + x_max) // 2
    cy = int(y_min * 0.6 + y_max * 0.4)
    return cx, cy


def sample_anchored_patch(img_np, anchor_cx, anchor_cy, patch_size=79, max_jitter=12, max_angle=10.0):
    # Guarantees that all touches of this finger share 65%-80% overlapping ridges.
    h, w = img_np.shape
    cx = anchor_cx + random.randint(-max_jitter, max_jitter)
    cy = anchor_cy + random.randint(-max_jitter, max_jitter)

    x0 = cx - patch_size // 2
    y0 = cy - patch_size // 2
    x0 = max(0, min(w - patch_size, x0))
    y0 = max(0, min(h - patch_size, y0))

    pad = 20
    crop_x0, crop_y0 = max(0, x0 - pad), max(0, y0 - pad)
    crop_x1, crop_y1 = min(w, x0 + patch_size + pad), min(h, y0 + patch_size + pad)
    crop = img_np[crop_y0:crop_y1, crop_x0:crop_x1]

    rot_crop = rotate(crop, random.uniform(-max_angle, max_angle), reshape=False, mode="nearest")

    ch, cw = rot_crop.shape
    sy, sx = (ch - patch_size) // 2, (cw - patch_size) // 2
    return rot_crop[sy:sy + patch_size, sx:sx + patch_size].astype(np.float32)


def apply_elastic_squish(patch, alpha=2.5, sigma=4.0):
    # Simulate skin squish
    h, w = patch.shape
    dx = gaussian_filter((np.random.rand(h, w) * 2 - 1), sigma) * alpha
    dy = gaussian_filter((np.random.rand(h, w) * 2 - 1), sigma) * alpha

    y, x = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    indices = np.reshape(y + dy, (-1, 1)), np.reshape(x + dx, (-1, 1))
    return map_coordinates(patch, indices, order=1, mode="nearest").reshape(h, w)


def elanize_tuned(patch):
    # Increase ridges thickness/glow
    h, w = patch.shape

    # Anguli ridges are black (0), capacitive ridges are white (255)
    ridge_img = 255.0 - patch
    ridge_img = np.clip(ridge_img, 0.0, 255.0)

    # Elastic skin squish
    ridge_img = apply_elastic_squish(ridge_img, alpha=random.uniform(1.8, 3.0), sigma=4.0)

    # Soften dilation
    dilated = maximum_filter(ridge_img, size=3)
    ridge_img = 0.85 * dilated + 0.15 * ridge_img
    ridge_img = gaussian_filter(ridge_img, sigma=0.75)

    # Contrast expansion
    norm = np.clip(ridge_img / 255.0, 0.0, 1.0)
    thickened = np.power(norm, 0.78) * 255.0

    # Ridge beading
    speckle = gaussian_filter(np.random.randn(h, w), sigma=1.1)
    speckle = (speckle - speckle.mean()) / (speckle.std() + 1e-6)
    # Additive brightness boost on ridge crests
    bead_boost = np.clip(speckle * 35.0, -10.0, 45.0)
    
    # Apply beads to strong ridges
    ridge_mask = np.clip((thickened - 60.0) / 120.0, 0.0, 1.0)
    thickened = thickened + bead_boost * ridge_mask

    # Occasional natural crease
    if random.random() < 0.28:
        vx = random.randint(12, w - 12)
        vy = random.randint(12, h - 12)
        rx = random.randint(4, 8)
        ry = random.randint(3, 6)
        y_grid, x_grid = np.ogrid[:h, :w]
        void_mask = (((x_grid - vx) ** 2) / (rx ** 2) + ((y_grid - vy) ** 2) / (ry ** 2)) <= 1.0
        thickened[void_mask] *= random.uniform(0.0, 0.15)

    # Off-Center contact liftoff
    y_grid, x_grid = np.ogrid[:h, :w]
    shift_x = random.uniform(-18.0, 18.0)
    shift_y = random.uniform(-18.0, 18.0)
    cx = (w - 1) / 2.0 + shift_x
    cy = (h - 1) / 2.0 + shift_y
    
    radius = random.uniform(46.0, 60.0)
    dist = np.sqrt((x_grid - cx) ** 2 + (y_grid - cy) ** 2)
    falloff = np.clip((radius + 15.0 - dist) / 18.0, 0.0, 1.0)
    falloff = 0.5 * (1.0 - np.cos(np.pi * falloff)) # smooth S-curve
    thickened *= falloff

    # Capacitive dielectric glow
    thickened = gaussian_filter(thickened, sigma=random.uniform(0.70, 0.95))

    # Ridge plateau saturation
    thickened = thickened * 1.22
    thickened = np.clip(thickened, 0.0, 255.0)

    # Point Spread Function (PSF)
    thickened = gaussian_filter(thickened, sigma=random.uniform(0.40, 0.60))

    # Clipping
    p2 = np.percentile(thickened, 2)
    p98 = np.percentile(thickened, 98)
    if p98 <= p2:
        p98 = p2 + 1.0
    stretched = (thickened - p2) * 255.0 / (p98 - p2)
    stretched = np.clip(stretched, 0.0, 255.0)

    return stretched.astype(np.uint8)


def generate_preview_grid(input_dir, preview_path="tuned_preview_grid.png"):
    # Generates a 2x3 composite preview
    pattern = os.path.join(input_dir, "**", "*.jpg")
    files = sorted(glob.glob(pattern, recursive=True))
    if not files:
        pattern = os.path.join(input_dir, "**", "*.png")
        files = sorted(glob.glob(pattern, recursive=True))
    if not files:
        print("No master images found.")
        return

    sample_file = files[0]
    print(f"Generating preview from: {sample_file}")
    img_np = np.array(Image.open(sample_file).convert("L"), dtype=np.float32)

    anchor_cx, anchor_cy = get_finger_anchor(img_np, patch_size=79)
    patches = [elanize_tuned(sample_anchored_patch(img_np, anchor_cx, anchor_cy, patch_size=79)) for _ in range(6)]

    row1 = np.hstack(patches[:3])
    row2 = np.hstack(patches[3:])
    grid = np.vstack([row1, row2])

    Image.fromarray(grid).save(preview_path)
    print(f"Preview saved to {preview_path}.")


def generate_dataset(input_dir, output_dir, num_fingers=1000, impressions_per_finger=10):
    os.makedirs(output_dir, exist_ok=True)
    pattern = os.path.join(input_dir, "**", "*.jpg")
    all_files = sorted(glob.glob(pattern, recursive=True))
    if not all_files:
        all_files = sorted(glob.glob(os.path.join(input_dir, "**", "*.png"), recursive=True))

    selected_files = all_files[: min(num_fingers, len(all_files))]
    print(f"Generating {len(selected_files)} fingers x {impressions_per_finger} impressions...")

    for finger_idx, file_path in enumerate(tqdm(selected_files, desc="Generating")):
        finger_dir = os.path.join(output_dir, f"finger_{finger_idx:05d}")
        os.makedirs(finger_dir, exist_ok=True)

        try:
            img_np = np.array(Image.open(file_path).convert("L"), dtype=np.float32)
        except Exception:
            continue

        anchor_cx, anchor_cy = get_finger_anchor(img_np, patch_size=79)
        for imp_idx in range(impressions_per_finger):
            raw = sample_anchored_patch(img_np, anchor_cx, anchor_cy, patch_size=79)
            elan = elanize_tuned(raw)
            out_filename = os.path.join(finger_dir, f"imp_{imp_idx:02d}.png")
            Image.fromarray(elan).save(out_filename)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Dataset generator")
    parser.add_argument("--input_dir", type=str, default="10K")
    parser.add_argument("--output_dir", type=str, default="dataset_79x79")
    parser.add_argument("--num_fingers", type=int, default=1000)
    parser.add_argument("--impressions", type=int, default=12)
    parser.add_argument("--preview", action="store_true")

    args = parser.parse_args()

    if args.preview:
        generate_preview_grid(args.input_dir)
    else:
        generate_dataset(args.input_dir, args.output_dir, args.num_fingers, args.impressions)
