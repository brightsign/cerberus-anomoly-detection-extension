#!/usr/bin/env python3
"""
Create a minimal calibration dataset using PIL (no OpenCV dependency).

This is a fallback for users who don't have video content yet.
For production use, extract frames from actual video content.
"""
import argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path


def create_synthetic_images(output_dir, num_images=100, size=(224, 224)):
    """
    Create synthetic test images for calibration using PIL.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print(f"Creating {num_images} synthetic calibration images...")
    print("⚠️  WARNING: Synthetic images are for TESTING ONLY")
    print("⚠️  For production, use actual video content frames")
    print()
    
    image_paths = []
    
    for i in range(num_images):
        # Create varied synthetic images
        if i % 4 == 0:
            # Gradient images
            img_array = np.zeros((size[1], size[0], 3), dtype=np.uint8)
            for y in range(size[1]):
                img_array[y, :, 0] = int((y / size[1]) * 255)
                img_array[y, :, 1] = int(((i % 20) / 20) * 255)
                img_array[y, :, 2] = int(((size[1] - y) / size[1]) * 255)
            img = Image.fromarray(img_array, 'RGB')
        elif i % 4 == 1:
            # Random noise
            img_array = np.random.randint(0, 256, (size[1], size[0], 3), dtype=np.uint8)
            img = Image.fromarray(img_array, 'RGB')
        elif i % 4 == 2:
            # Solid colors with shapes
            base_color = ((i * 7) % 256, (i * 13) % 256, (i * 19) % 256)
            img = Image.new('RGB', size, base_color)
            draw = ImageDraw.Draw(img)
            draw.ellipse([size[0]//4, size[1]//4, 3*size[0]//4, 3*size[1]//4], 
                        fill=(255, 255, 255))
        else:
            # Checkerboard patterns
            img = Image.new('RGB', size, (0, 0, 0))
            draw = ImageDraw.Draw(img)
            square_size = 16
            for y in range(0, size[1], square_size):
                for x in range(0, size[0], square_size):
                    if ((x // square_size) + (y // square_size)) % 2 == 0:
                        draw.rectangle([x, y, x+square_size, y+square_size], 
                                     fill=(200, 200, 200))
        
        # Add text
        draw = ImageDraw.Draw(img)
        try:
            # Try to use a font, fall back to default if not available
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20)
        except:
            font = ImageFont.load_default()
        draw.text((10, 10), f"Cal {i}", fill=(255, 255, 0), font=font)
        
        # Save image
        output_path = output_dir / f"synthetic_{i:04d}.jpg"
        img.save(str(output_path), 'JPEG', quality=95)
        image_paths.append(output_path)
        
        if (i + 1) % 20 == 0:
            print(f"  Created {i + 1}/{num_images} images...", end='\r')
    
    print(f"  Created {num_images}/{num_images} images... Done!")
    return image_paths


def main():
    parser = argparse.ArgumentParser(
        description='Create synthetic calibration dataset (for testing only)'
    )
    parser.add_argument(
        '--output-dir',
        default='toolkit/calibration_images',
        help='Output directory for calibration images'
    )
    parser.add_argument(
        '--dataset-file',
        default='toolkit/calibration_dataset.txt',
        help='Output dataset file'
    )
    parser.add_argument(
        '--num-images',
        type=int,
        default=100,
        help='Number of synthetic images to create (default: 100)'
    )
    
    args = parser.parse_args()
    
    # Create synthetic images
    image_paths = create_synthetic_images(
        args.output_dir,
        num_images=args.num_images
    )
    
    # Write dataset file with relative paths (for Docker compatibility)
    dataset_file = Path(args.dataset_file)
    dataset_file.parent.mkdir(parents=True, exist_ok=True)
    
    # Get relative path from toolkit/ directory (Docker mount point)
    with open(dataset_file, 'w') as f:
        for path in image_paths:
            # Write path relative to toolkit/ directory for Docker
            # Docker mounts toolkit/ as /workspace, so we need paths like:
            # /workspace/calibration_images/synthetic_0000.jpg
            rel_path = path.relative_to(Path('toolkit'))
            f.write(f"/workspace/{rel_path}\n")
    
    print(f"\nSynthetic calibration dataset created!")
    print(f"  Images: {len(image_paths)}")
    print(f"  Directory: {args.output_dir}")
    print(f"  Dataset file: {dataset_file}")
    print()
    print("⚠️  IMPORTANT:")
    print("   Synthetic images are for TESTING the pipeline only.")
    print("   For production deployment, create a real calibration dataset:")
    print()
    print("   python tools/create_calibration_dataset.py \\")
    print("       --videos video1.mp4 video2.mp4 \\")
    print("       --frames-per-video 100")
    print()
    print("Next step:")
    print("  ./compile-models")


if __name__ == '__main__':
    main()
