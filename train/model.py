import torch
import torch.nn as nn
import torch.nn.functional as F


class ResidualBlock(nn.Module):
    def __init__(self, channels):
        super().__init__()
        self.conv = nn.Sequential(
            nn.Conv2d(channels, channels, kernel_size=3, stride=1, padding=1, bias=False),
            nn.BatchNorm2d(channels),
            nn.PReLU(channels),
            nn.Conv2d(channels, channels, kernel_size=3, stride=1, padding=1, bias=False),
            nn.BatchNorm2d(channels),
        )
        self.prelu = nn.PReLU(channels)

    def forward(self, x):
        return self.prelu(x + self.conv(x))


class DownsampleBlock(nn.Module):
    def __init__(self, in_c, out_c):
        super().__init__()
        self.conv = nn.Sequential(
            nn.Conv2d(in_c, out_c, kernel_size=3, stride=2, padding=1, bias=False),
            nn.BatchNorm2d(out_c),
            nn.PReLU(out_c),
        )

    def forward(self, x):
        return self.conv(x)


class TinyNet(nn.Module):
    # Outputs a unit-length 128-D embedding
    def __init__(self, embedding_dim=128):
        super().__init__()
        # Preserve high-frequency ridge detail (stride 1)
        # Input [B, 1, 79, 79]
        # Output: [B, 16, 79, 79]
        self.init_conv = nn.Sequential(
            nn.Conv2d(1, 16, kernel_size=3, stride=1, padding=1, bias=False),
            nn.BatchNorm2d(16),
            nn.PReLU(16),
        )

        # 79x79 -> 40x40 (channels: 16 -> 32)
        self.stage1 = nn.Sequential(
            DownsampleBlock(16, 32),
            ResidualBlock(32),
        )

        # 40x40 -> 20x20 (channels: 32 -> 64)
        self.stage2 = nn.Sequential(
            DownsampleBlock(32, 64),
            ResidualBlock(64),
        )

        # 20x20 -> 10x10 (channels: 64 -> 128)
        self.stage3 = nn.Sequential(
            DownsampleBlock(64, 128),
            ResidualBlock(128),
        )

        # 10x10 -> 5x5 (channels: 128 -> 128)
        self.stage4 = nn.Sequential(
            DownsampleBlock(128, 128),
            ResidualBlock(128),
        )

        # Pooling and embedding head
        self.gap = nn.AdaptiveAvgPool2d((1, 1))
        self.flatten = nn.Flatten()
        self.fc = nn.Linear(128, embedding_dim, bias=False)
        self.bn_head = nn.BatchNorm1d(embedding_dim)

    def forward(self, x):
        x = self.init_conv(x)
        x = self.stage1(x)
        x = self.stage2(x)
        x = self.stage3(x)
        x = self.stage4(x)

        x = self.gap(x)
        x = self.flatten(x)
        x = self.fc(x)
        x = self.bn_head(x)

        # L2-normalize to unit hypersphere (cosine comp)
        embeddings = F.normalize(x, p=2, dim=1)
        return embeddings


class ArcMarginProduct(nn.Module):
    # ArcFace loss head
    # cos(theta + m) with scale s
    def __init__(self, in_features, out_features, s=32.0, m=0.35):
        super().__init__()
        self.in_features = in_features
        self.out_features = out_features
        self.s = s
        self.m = m
        self.weight = nn.Parameter(torch.FloatTensor(out_features, in_features))
        nn.init.xavier_uniform_(self.weight)

        self.cos_m = torch.cos(torch.tensor(m))
        self.sin_m = torch.sin(torch.tensor(m))
        self.th = torch.cos(torch.tensor(3.141592653589793 - m))
        self.mm = torch.sin(torch.tensor(3.141592653589793 - m)) * m

    def forward(self, input_features, label, current_m=None):
        if current_m is None:
            current_m = self.m

        # Normalized cosine logits
        cosine = F.linear(input_features, F.normalize(self.weight, dim=1))

        # If in warmup phase (m <= 0), use standard normalized softmax
        if current_m <= 0.0:
            return cosine * self.s, cosine

        cos_m = torch.cos(torch.tensor(current_m, device=cosine.device))
        sin_m = torch.sin(torch.tensor(current_m, device=cosine.device))
        th = torch.cos(torch.tensor(3.141592653589793 - current_m, device=cosine.device))
        mm = torch.sin(torch.tensor(3.141592653589793 - current_m, device=cosine.device)) * current_m

        sine = torch.sqrt(torch.clamp(1.0 - torch.pow(cosine, 2), 1e-7, 1.0))
        phi = cosine * cos_m - sine * sin_m
        phi = torch.where(cosine > th, phi, cosine - mm)

        one_hot = torch.zeros(cosine.size(), device=input_features.device)
        one_hot.scatter_(1, label.view(-1, 1).long(), 1)
        output = (one_hot * phi) + ((1.0 - one_hot) * cosine)
        output *= self.s

        # Return both the penalized logits (for loss) and raw cosines (for accuracy)
        return output, cosine