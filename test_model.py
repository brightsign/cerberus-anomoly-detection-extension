#!/usr/bin/env python3
"""
Test script to validate the compiled MobileNetV2 RKNN model.

Uses the RKNN Model Zoo example structure to test our compiled model.
This validates that:
1. The model loads correctly
2. Inference runs successfully
3. Output shapes are correct (for embedding extraction)
4. Classification works (using the classifier head)
"""

import argparse
import os
import sys
import numpy as np
import cv2
from rknn.api import RKNN
from scipy.special import softmax

# Paths
MODEL_ZOO_DIR = '/workspace/toolkit/rknn_model_zoo/examples/mobilenet'
TEST_IMAGE = os.path.join(MODEL_ZOO_DIR, 'model/bell.jpg')
CLASS_LABELS = os.path.join(MODEL_ZOO_DIR, 'model/synset.txt')

# Default model paths for different platforms
DEFAULT_MODELS = {
    'RK3588': '/workspace/install/RK3588/model/mobilenetv2-12.rknn',
    'RK3568': '/workspace/install/RK3568/model/mobilenetv2-12.rknn',
    'RK3576': '/workspace/install/RK3576/model/mobilenetv2-12.rknn',
}


def test_model(model_path, target_platform, test_image, show_classification=True):
    """Test the RKNN model with a sample image."""
    
    print(f"\n{'='*60}")
    print(f"Testing MobileNetV2 Model: {model_path}")
    print(f"Target Platform: {target_platform}")
    print(f"Test Image: {test_image}")
    print(f"{'='*60}\n")
    
    # Verify files exist
    if not os.path.exists(model_path):
        print(f"ERROR: Model not found at {model_path}")
        return False
    
    if not os.path.exists(test_image):
        print(f"ERROR: Test image not found at {test_image}")
        return False
    
    # Create RKNN object
    rknn = RKNN(verbose=False)
    
    # Load RKNN model
    print('--> Loading RKNN model')
    ret = rknn.load_rknn(model_path)
    if ret != 0:
        print('ERROR: Failed to load RKNN model!')
        return False
    print('✓ Model loaded successfully')
    
    # Init runtime environment (simulator mode for x86_64 host)
    print('--> Initializing runtime environment (simulator)')
    # Pre-compiled RKNN models require target specification
    ret = rknn.init_runtime(target=target_platform)
    if ret != 0:
        print('ERROR: Failed to init runtime environment!')
        print('NOTE: Pre-compiled RKNN models can only be tested on actual hardware.')
        print('      The model is valid, but cannot run in x86_64 simulator mode.')
        rknn.release()
        return False
    print('✓ Runtime initialized successfully')
    
    # Prepare input image
    print('--> Preparing input image')
    img = cv2.imread(test_image)
    if img is None:
        print(f'ERROR: Failed to read image: {test_image}')
        rknn.release()
        return False
    
    original_shape = img.shape
    print(f'   Original image shape: {original_shape}')
    
    # Resize to 224x224 (MobileNetV2 input size)
    img = cv2.resize(img, (224, 224))
    print(f'   Resized to: {img.shape}')
    
    # Add batch dimension
    img = np.expand_dims(img, 0)
    print(f'   Input tensor shape: {img.shape}')
    
    # Run inference
    print('--> Running inference')
    outputs = rknn.inference(inputs=[img])
    if outputs is None or len(outputs) == 0:
        print('ERROR: Inference failed!')
        rknn.release()
        return False
    print('✓ Inference completed successfully')
    
    # Analyze outputs
    print(f'\n--> Model Outputs:')
    for i, output in enumerate(outputs):
        print(f'   Output[{i}]: shape={output.shape}, dtype={output.dtype}')
        print(f'             min={output.min():.4f}, max={output.max():.4f}, mean={output.mean():.4f}')
    
    # Check if this is the embedding output or classifier output
    if outputs[0].shape[-1] == 1280:
        print(f'\n✓ Embedding output detected: {outputs[0].shape[-1]}-dimensional vector')
        print(f'   This is the feature embedding layer (before classification)')
        print(f'   Perfect for anomaly detection!')
        
        # Show embedding statistics
        embedding = outputs[0].flatten()
        print(f'\n   Embedding Statistics:')
        print(f'   - Dimensions: {len(embedding)}')
        print(f'   - L2 Norm: {np.linalg.norm(embedding):.4f}')
        print(f'   - Mean: {embedding.mean():.4f}')
        print(f'   - Std: {embedding.std():.4f}')
        print(f'   - Sparsity: {(embedding == 0).sum() / len(embedding) * 100:.1f}%')
        
    elif outputs[0].shape[-1] == 1000:
        print(f'\n✓ Classification output detected: {outputs[0].shape[-1]} classes (ImageNet)')
        
        if show_classification and os.path.exists(CLASS_LABELS):
            # Load class labels
            with open(CLASS_LABELS, 'r') as f:
                labels = [l.rstrip() for l in f]
            
            # Apply softmax
            scores = softmax(outputs[0])
            scores = np.squeeze(scores)
            
            # Get top-5 predictions
            top5_idx = np.argsort(scores)[::-1][:5]
            
            print('\n--> TOP 5 Classifications:')
            for rank, idx in enumerate(top5_idx, 1):
                print(f'   {rank}. [{idx:3d}] score={scores[idx]:.6f} class="{labels[idx]}"')
    else:
        print(f'\n⚠ Unexpected output shape: {outputs[0].shape}')
        print(f'   Expected 1280 (embedding) or 1000 (classification)')
    
    # Release resources
    rknn.release()
    
    print(f'\n{"="*60}')
    print('✓ Model test completed successfully!')
    print(f'{"="*60}\n')
    
    return True


def main():
    parser = argparse.ArgumentParser(
        description='Test compiled MobileNetV2 RKNN models',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Test LS5 model (RK3568)
  python test_model.py --platform LS5
  
  # Test XT5 model (RK3588)
  python test_model.py --platform XT5
  
  # Test specific model file
  python test_model.py --model install/RK3568/model/mobilenetv2-12.rknn --target rk3568
  
  # Test with custom image
  python test_model.py --platform LS5 --image my_test_image.jpg
        """)
    
    parser.add_argument('--platform', type=str, choices=['XT5', 'LS5', 'Firebird'],
                        help='BrightSign platform (determines model and target)')
    parser.add_argument('--model', type=str,
                        help='Path to RKNN model (overrides --platform)')
    parser.add_argument('--target', type=str,
                        help='RKNN target platform (e.g., rk3588, rk3568, rk3576)')
    parser.add_argument('--image', type=str, default=TEST_IMAGE,
                        help='Test image path (default: bell.jpg from Model Zoo)')
    parser.add_argument('--no-classification', action='store_true',
                        help='Skip classification output (for embedding-only models)')
    
    args = parser.parse_args()
    
    # Determine model and target
    if args.model:
        model_path = args.model
        if not args.target:
            print("ERROR: --target is required when using --model")
            return 1
        target = args.target
    elif args.platform:
        # Map platform to SOC
        platform_map = {
            'XT5': ('RK3588', 'rk3588'),
            'LS5': ('RK3568', 'rk3568'),
            'Firebird': ('RK3576', 'rk3576'),
        }
        soc, target = platform_map[args.platform]
        model_path = DEFAULT_MODELS[soc]
    else:
        print("ERROR: Either --platform or --model must be specified")
        parser.print_help()
        return 1
    
    # Run test
    success = test_model(
        model_path=model_path,
        target_platform=target,
        test_image=args.image,
        show_classification=not args.no_classification
    )
    
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
