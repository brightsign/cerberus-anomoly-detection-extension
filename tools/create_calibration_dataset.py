#!/usr/bin/env python3
"""
Create calibration dataset for MobileNetV2 quantization.

This script extracts diverse frames from video files to use as calibration
data for INT8 quantization of the MobileNetV2 model.
"""
import argparse
import os
import sys
import cv2
import numpy as np
from pathlib import Path


def extract_frames(video_path, output_dir, num_frames=100, resize=(224, 224)):
    """
    Extract evenly-spaced frames from a video file.
    
    Args:
        video_path: Path to input video
        output_dir: Directory to save extracted frames
        num_frames: Number of frames to extract
        resize: Resize dimensions (width, height)
    
    Returns:
        List of saved frame paths
    """
    video_name = Path(video_path).stem
    cap = cv2.VideoCapture(str(video_path))
    
    if not cap.isOpened():
        print(f"Error: Could not open video {video_path}")
        return []
    
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    duration = total_frames / fps if fps > 0 else 0
    
    print(f"Processing {video_path}:")
    print(f"  Total frames: {total_frames}")
    print(f"  FPS: {fps:.2f}")
    print(f"  Duration: {duration:.2f}s")
    
    # Calculate frame interval
    if total_frames < num_frames:
        print(f"  Warning: Video has fewer frames ({total_frames}) than requested ({num_frames})")
        num_frames = total_frames
        frame_interval = 1
    else:
        frame_interval = total_frames // num_frames
    
    saved_paths = []
    frame_idx = 0
    saved_count = 0
    
    while saved_count < num_frames:
        cap.set(cv2.CAP_PROP_POS_FRAMES, frame_idx)
        ret, frame = cap.read()
        
        if not ret:
            break
        
        # Resize frame
        if resize:
            frame = cv2.resize(frame, resize)
        
        # Save frame
        output_path = output_dir / f"{video_name}_frame_{saved_count:04d}.jpg"
        cv2.imwrite(str(output_path), frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
        saved_paths.append(output_path)
        
        saved_count += 1
        frame_idx += frame_interval
        
        if saved_count % 10 == 0:
            print(f"  Extracted {saved_count}/{num_frames} frames...", end='\r')
    
    cap.release()
    print(f"  Extracted {saved_count}/{num_frames} frames... Done!")
    
    return saved_paths


def main():
    parser = argparse.ArgumentParser(
        description='Create calibration dataset from video files for MobileNetV2 quantization'
    )
    parser.add_argument(
        '--videos',
        nargs='+',
        help='One or more video files to extract frames from'
    )
    parser.add_argument(
        '--output-dir',
        default='toolkit/calibration_images',
        help='Output directory for extracted frames (default: toolkit/calibration_images)'
    )
    parser.add_argument(
        '--dataset-file',
        default='toolkit/calibration_dataset.txt',
        help='Output dataset file listing all images (default: toolkit/calibration_dataset.txt)'
    )
    parser.add_argument(
        '--frames-per-video',
        type=int,
        default=100,
        help='Number of frames to extract per video (default: 100)'
    )
    parser.add_argument(
        '--total-frames',
        type=int,
        help='Total frames to extract across all videos (overrides --frames-per-video)'
    )
    parser.add_argument(
        '--resize',
        default='224,224',
        help='Resize dimensions as "width,height" (default: 224,224)'
    )
    
    args = parser.parse_args()
    
    # Check if videos provided
    if not args.videos:
        print("Error: No video files specified. Use --videos to provide input videos.")
        print("\nExample:")
        print("  python tools/create_calibration_dataset.py --videos video1.mp4 video2.mp4")
        sys.exit(1)
    
    # Parse resize dimensions
    try:
        resize = tuple(map(int, args.resize.split(',')))
        if len(resize) != 2:
            raise ValueError
    except ValueError:
        print(f"Error: Invalid resize format '{args.resize}'. Use 'width,height' (e.g., '224,224')")
        sys.exit(1)
    
    # Create output directory
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Calculate frames per video
    if args.total_frames:
        frames_per_video = args.total_frames // len(args.videos)
        print(f"Extracting total of {args.total_frames} frames ({frames_per_video} per video)")
    else:
        frames_per_video = args.frames_per_video
        print(f"Extracting {frames_per_video} frames per video")
    
    # Extract frames from all videos
    all_frame_paths = []
    for video_path in args.videos:
        if not os.path.exists(video_path):
            print(f"Warning: Video not found: {video_path}")
            continue
        
        frame_paths = extract_frames(
            video_path,
            output_dir,
            num_frames=frames_per_video,
            resize=resize
        )
        all_frame_paths.extend(frame_paths)
    
    if not all_frame_paths:
        print("\nError: No frames extracted. Check your video files.")
        sys.exit(1)
    
    # Write dataset file (relative paths for Docker compatibility)
    dataset_file = Path(args.dataset_file)
    dataset_file.parent.mkdir(parents=True, exist_ok=True)
    
    with open(dataset_file, 'w') as f:
        for path in all_frame_paths:
            # Write path relative to toolkit/ directory for Docker
            # Docker mounts toolkit/ as /workspace, so we need paths like:
            # /workspace/calibration_images/video_frame_0000.jpg
            rel_path = path.relative_to(Path('toolkit'))
            f.write(f"/workspace/{rel_path}\n")
    
    print(f"\nCalibration dataset created successfully!")
    print(f"  Total frames: {len(all_frame_paths)}")
    print(f"  Output directory: {output_dir}")
    print(f"  Dataset file: {dataset_file}")
    print(f"\nNext step:")
    print(f"  ./compile-models")


if __name__ == '__main__':
    main()
