#!/usr/bin/env python3
"""
Extract 1280-dim embedding layer from MobileNetV2 ONNX model.

This script modifies the ONNX model to output the embedding layer
(after global average pooling) instead of the 1000-class classifier.
"""

import onnx
from onnx import helper, numpy_helper
import sys
import os

def inspect_model(model_path):
    """Show model structure to help find embedding layer."""
    model = onnx.load(model_path)
    
    print("=" * 60)
    print("Model Structure Analysis")
    print("=" * 60)
    
    print("\nModel Inputs:")
    for inp in model.graph.input:
        print(f"  - {inp.name}: {[d.dim_value for d in inp.type.tensor_type.shape.dim]}")
    
    print("\nModel Outputs:")
    for out in model.graph.output:
        print(f"  - {out.name}: {[d.dim_value for d in out.type.tensor_type.shape.dim]}")
    
    print("\nLooking for GlobalAveragePool or Pooling nodes:")
    pool_nodes = []
    for node in model.graph.node:
        if 'pool' in node.op_type.lower() or 'average' in node.op_type.lower():
            pool_nodes.append(node)
            print(f"  ✓ {node.name}:")
            print(f"      Type: {node.op_type}")
            print(f"      Inputs: {node.input}")
            print(f"      Outputs: {node.output}")
    
    if not pool_nodes:
        print("  (No explicit pooling nodes found - may be implicit in structure)")
        print("\nSearching for nodes before final classifier:")
        # Look for nodes that feed into a Gemm/MatMul (classifier)
        classifier_inputs = set()
        for node in model.graph.node:
            if node.op_type in ['Gemm', 'MatMul', 'Conv']:
                # Check output shape
                for out_name in node.output:
                    # If this goes to model output, its inputs are embedding candidates
                    if any(out_name == model_out.name for model_out in model.graph.output):
                        classifier_inputs.update(node.input)
                        print(f"  → Classifier node: {node.name} ({node.op_type})")
                        print(f"      Inputs (potential embeddings): {node.input}")
        
        print("\nPotential embedding tensors:")
        for tensor_name in classifier_inputs:
            # Find the node that produces this tensor
            for node in model.graph.node:
                if tensor_name in node.output:
                    print(f"  - {tensor_name} (from {node.name}: {node.op_type})")
    
    return pool_nodes

def extract_embedding(input_path, output_path, embedding_tensor_name=None):
    """
    Modify ONNX model to output embedding layer.
    
    Args:
        input_path: Path to original ONNX model
        output_path: Path to save modified model
        embedding_tensor_name: Name of tensor to use as output (auto-detect if None)
    """
    model = onnx.load(input_path)
    
    # Auto-detect if not specified
    if embedding_tensor_name is None:
        print("\nAuto-detecting embedding layer...")
        
        # Strategy 1: Find GlobalAveragePool output
        for node in model.graph.node:
            if node.op_type == 'GlobalAveragePool':
                embedding_tensor_name = node.output[0]
                print(f"  Found GlobalAveragePool output: {embedding_tensor_name}")
                break
        
        # Strategy 2: Find last node before classifier
        if embedding_tensor_name is None:
            # Get classifier input
            for node in model.graph.node:
                if node.op_type in ['Gemm', 'MatMul']:
                    for out_name in node.output:
                        if any(out_name == model_out.name for model_out in model.graph.output):
                            # This is the classifier, use its first input
                            embedding_tensor_name = node.input[0]
                            print(f"  Found classifier input: {embedding_tensor_name}")
                            break
                if embedding_tensor_name:
                    break
        
        if embedding_tensor_name is None:
            print("✗ Could not auto-detect embedding layer!")
            print("  Please specify the tensor name manually.")
            sys.exit(1)
    
    # Find the shape of the embedding tensor
    embedding_shape = None
    for value_info in model.graph.value_info:
        if value_info.name == embedding_tensor_name:
            embedding_shape = [d.dim_value for d in value_info.type.tensor_type.shape.dim]
            break
    
    # If not in value_info, try to infer from node outputs
    if embedding_shape is None:
        for node in model.graph.node:
            if embedding_tensor_name in node.output:
                # Common patterns for MobileNetV2
                if node.op_type == 'GlobalAveragePool':
                    embedding_shape = [1, 1280, 1, 1]
                elif 'flatten' in node.op_type.lower():
                    embedding_shape = [1, 1280]
                break
    
    if embedding_shape is None:
        # Default for MobileNetV2
        print("  Warning: Could not determine shape, using default [1, 1280, 1, 1]")
        embedding_shape = [1, 1280, 1, 1]
    
    print(f"\n✓ Using embedding tensor: {embedding_tensor_name}")
    print(f"  Shape: {embedding_shape}")
    
    # Create new output tensor
    embedding_output = helper.make_tensor_value_info(
        embedding_tensor_name,
        onnx.TensorProto.FLOAT,
        embedding_shape
    )
    
    # Replace model outputs
    del model.graph.output[:]
    model.graph.output.append(embedding_output)
    
    # Optionally add a flatten if needed
    if len(embedding_shape) == 4 and embedding_shape[2] == 1 and embedding_shape[3] == 1:
        print("  Adding Flatten node to convert [1, 1280, 1, 1] → [1, 1280]")
        flatten_node = helper.make_node(
            'Flatten',
            inputs=[embedding_tensor_name],
            outputs=['embedding_flat'],
            axis=1
        )
        model.graph.node.append(flatten_node)
        
        # Update output
        flat_output = helper.make_tensor_value_info(
            'embedding_flat',
            onnx.TensorProto.FLOAT,
            [1, 1280]
        )
        del model.graph.output[:]
        model.graph.output.append(flat_output)
        print("  Final output shape: [1, 1280]")
    
    # Save modified model
    onnx.save(model, output_path)
    print(f"\n✓ Saved modified model to: {output_path}")
    
    # Verify
    modified = onnx.load(output_path)
    onnx.checker.check_model(modified)
    print("✓ Model validation passed")
    
    return True

def main():
    if len(sys.argv) < 2 or sys.argv[1] in ['-h', '--help']:
        print("Usage:")
        print("  python3 extract_embedding_layer.py <model.onnx> [output.onnx] [tensor_name]")
        print("")
        print("Examples:")
        print("  # Auto-detect embedding layer")
        print("  python3 extract_embedding_layer.py mobilenetv2-12.onnx")
        print("")
        print("  # Specify output path")
        print("  python3 extract_embedding_layer.py mobilenetv2-12.onnx mobilenetv2-embedding.onnx")
        print("")
        print("  # Specify embedding tensor name")
        print("  python3 extract_embedding_layer.py mobilenetv2-12.onnx output.onnx features.18")
        print("")
        print("  # Inspect model structure only")
        print("  python3 extract_embedding_layer.py mobilenetv2-12.onnx --inspect")
        sys.exit(0)
    
    input_path = sys.argv[1]
    
    if not os.path.exists(input_path):
        print(f"✗ Error: Input file not found: {input_path}")
        sys.exit(1)
    
    # Inspect mode
    if len(sys.argv) > 2 and sys.argv[2] == '--inspect':
        inspect_model(input_path)
        print("\nUse the tensor name from above as the third argument to extract it.")
        return
    
    # Default output path
    base_name = os.path.splitext(input_path)[0]
    output_path = sys.argv[2] if len(sys.argv) > 2 else f"{base_name}-embedding.onnx"
    
    # Optional tensor name
    tensor_name = sys.argv[3] if len(sys.argv) > 3 else None
    
    print(f"Input:  {input_path}")
    print(f"Output: {output_path}")
    
    # First inspect to show structure
    pool_nodes = inspect_model(input_path)
    
    # Extract embedding
    print("\n" + "=" * 60)
    print("Extracting Embedding Layer")
    print("=" * 60)
    extract_embedding(input_path, output_path, tensor_name)
    
    print("\n" + "=" * 60)
    print("✓ Done!")
    print("=" * 60)
    print("\nNext steps:")
    print("  1. Verify the model:")
    print(f"     python3 -c 'import onnx; onnx.checker.check_model(\"{output_path}\")'")
    print("  2. Recompile to RKNN:")
    print("     ./compile-models")
    print("  3. Test on device")

if __name__ == '__main__':
    main()
