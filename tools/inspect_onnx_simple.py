#!/usr/bin/env python3
"""Simple ONNX model inspector to find embedding layer."""
import onnx
import sys

if len(sys.argv) < 2:
    print("Usage: python3 inspect_onnx_simple.py <model.onnx>")
    sys.exit(1)

model = onnx.load(sys.argv[1])

print("=" * 70)
print("ONNX Model Structure")
print("=" * 70)

print("\nInput:")
for inp in model.graph.input:
    if inp.name in [i.name for i in model.graph.initializer]:
        continue  # Skip weights/biases
    shape = [d.dim_value for d in inp.type.tensor_type.shape.dim]
    print(f"  {inp.name}: {shape}")

print("\nOutput:")
for out in model.graph.output:
    shape = [d.dim_value for d in out.type.tensor_type.shape.dim]
    print(f"  {out.name}: {shape}")

print("\nAll Nodes (showing layer flow):")
for i, node in enumerate(model.graph.node):
    print(f"{i:3d}. {node.op_type:20s} {node.name or '(unnamed)'}")
    for inp in node.input:
        if inp not in [init.name for init in model.graph.initializer]:
            print(f"       ← {inp}")
    for out in node.output:
        print(f"       → {out}")

print("\n" + "=" * 70)
print("Looking for embedding layer...")
print("=" * 70)

# Find the last conv before classifier
last_conv_output = None
for node in model.graph.node:
    if node.op_type == 'Conv':
        # Check if this feeds into a Flatten or Reshape before classifier
        last_conv_output = node.output[0]
        print(f"\nFound Conv node: {node.name}")
        print(f"  Output: {node.output[0]}")

# Find classifier input (the embedding)
for node in model.graph.node:
    if 'classifier' in node.name.lower() or node.op_type == 'Gemm':
        print(f"\nFound Classifier node: {node.name} ({node.op_type})")
        print(f"  Inputs: {node.input}")
        print(f"  → First input is likely the 1280-dim embedding: {node.input[0]}")
        
        # Try to find the node that produces this
        embedding_tensor = node.input[0]
        for prev_node in model.graph.node:
            if embedding_tensor in prev_node.output:
                print(f"\nEmbedding produced by: {prev_node.name} ({prev_node.op_type})")
                print(f"  Inputs: {prev_node.input}")
                print(f"  Outputs: {prev_node.output}")
                break

print("\n" + "=" * 70)
print("Recommendation:")
print("=" * 70)
print("\nTo extract 1280-dim embeddings, modify the model to output")
print("the tensor that feeds into the classifier (before the final")
print("1000-class linear layer).")
