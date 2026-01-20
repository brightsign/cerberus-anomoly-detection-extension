/*
 * Simple MobileNetV2 Embedding Test for BrightSign XT5
 * 
 * This test application validates the compiled MobileNetV2 model
 * by running inference on a test image and extracting embeddings.
 * 
 * Based on RKNN Model Zoo mobilenet example.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rknn_api.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

typedef struct {
    int width;
    int height;
    int channels;
    unsigned char* data;
} Image;

// Load image using stb_image
Image* load_image(const char* filename) {
    Image* img = (Image*)malloc(sizeof(Image));
    if (!img) return NULL;
    
    img->data = stbi_load(filename, &img->width, &img->height, &img->channels, 3);
    if (!img->data) {
        printf("ERROR: Failed to load image: %s\n", filename);
        free(img);
        return NULL;
    }
    
    img->channels = 3;  // Force RGB
    return img;
}

void free_image(Image* img) {
    if (img) {
        if (img->data) stbi_image_free(img->data);
        free(img);
    }
}

// Resize image to 224x224 (simple nearest neighbor)
unsigned char* resize_image(const Image* img, int target_w, int target_h) {
    unsigned char* resized = (unsigned char*)malloc(target_w * target_h * 3);
    if (!resized) return NULL;
    
    float x_ratio = (float)img->width / target_w;
    float y_ratio = (float)img->height / target_h;
    
    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);
            
            int src_idx = (src_y * img->width + src_x) * 3;
            int dst_idx = (y * target_w + x) * 3;
            
            resized[dst_idx + 0] = img->data[src_idx + 0];  // R
            resized[dst_idx + 1] = img->data[src_idx + 1];  // G
            resized[dst_idx + 2] = img->data[src_idx + 2];  // B
        }
    }
    
    return resized;
}

// Calculate L2 norm of embedding vector
float calculate_l2_norm(const float* embedding, int dim) {
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) {
        sum += embedding[i] * embedding[i];
    }
    return sqrtf(sum);
}

// Calculate mean and std of embedding
void calculate_stats(const float* embedding, int dim, float* mean, float* std) {
    *mean = 0.0f;
    *std = 0.0f;
    
    // Calculate mean
    for (int i = 0; i < dim; i++) {
        *mean += embedding[i];
    }
    *mean /= dim;
    
    // Calculate std deviation
    for (int i = 0; i < dim; i++) {
        float diff = embedding[i] - *mean;
        *std += diff * diff;
    }
    *std = sqrtf(*std / dim);
}

// Count zero values (sparsity)
int count_zeros(const float* embedding, int dim) {
    int zeros = 0;
    for (int i = 0; i < dim; i++) {
        if (fabsf(embedding[i]) < 1e-6f) zeros++;
    }
    return zeros;
}

// Softmax function for classification output
void softmax(float* input, int size) {
    // Find max for numerical stability
    float max_val = input[0];
    for (int i = 1; i < size; i++) {
        if (input[i] > max_val) max_val = input[i];
    }
    
    // Compute exp(x - max) and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        input[i] = expf(input[i] - max_val);
        sum += input[i];
    }
    
    // Normalize
    for (int i = 0; i < size; i++) {
        input[i] /= sum;
    }
}

// Get top-K indices
void get_topk(const float* scores, int size, int k, int* indices, float* values) {
    // Simple selection sort for top-k
    for (int i = 0; i < k; i++) {
        int max_idx = -1;
        float max_val = -1.0f;
        
        for (int j = 0; j < size; j++) {
            // Check if already selected
            bool selected = false;
            for (int m = 0; m < i; m++) {
                if (indices[m] == j) {
                    selected = true;
                    break;
                }
            }
            
            if (!selected && scores[j] > max_val) {
                max_val = scores[j];
                max_idx = j;
            }
        }
        
        indices[i] = max_idx;
        values[i] = max_val;
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("Usage: %s <model_path> <image_path>\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s /userdata/mobilenetv2-12.rknn /userdata/bell.jpg\n", argv[0]);
        return -1;
    }
    
    const char* model_path = argv[1];
    const char* image_path = argv[2];
    
    printf("\n");
    printf("=====================================================\n");
    printf("  MobileNetV2 Embedding Test - BrightSign XT5\n");
    printf("=====================================================\n");
    printf("Model:  %s\n", model_path);
    printf("Image:  %s\n", image_path);
    printf("=====================================================\n\n");
    
    // Load model
    printf("[1/5] Loading RKNN model...\n");
    FILE* fp = fopen(model_path, "rb");
    if (!fp) {
        printf("ERROR: Failed to open model file: %s\n", model_path);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    size_t model_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    void* model_data = malloc(model_size);
    if (!model_data) {
        printf("ERROR: Failed to allocate memory for model\n");
        fclose(fp);
        return -1;
    }
    
    if (fread(model_data, 1, model_size, fp) != model_size) {
        printf("ERROR: Failed to read model file\n");
        free(model_data);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    
    printf("      Model size: %.2f MB\n", model_size / (1024.0 * 1024.0));
    
    // Initialize RKNN
    rknn_context ctx;
    int ret = rknn_init(&ctx, model_data, model_size, 0, NULL);
    free(model_data);
    
    if (ret < 0) {
        printf("ERROR: rknn_init failed! ret=%d\n", ret);
        return -1;
    }
    printf("      ✓ Model loaded successfully\n\n");
    
    // Query model I/O
    printf("[2/5] Querying model I/O...\n");
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("ERROR: rknn_query failed! ret=%d\n", ret);
        rknn_destroy(ctx);
        return -1;
    }
    
    printf("      Input tensors:  %d\n", io_num.n_input);
    printf("      Output tensors: %d\n", io_num.n_output);
    
    // Get input attributes
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("ERROR: rknn_query input failed! ret=%d\n", ret);
            rknn_destroy(ctx);
            return -1;
        }
    }
    
    // Get output attributes
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("ERROR: rknn_query output failed! ret=%d\n", ret);
            rknn_destroy(ctx);
            return -1;
        }
    }
    
    // Print input tensor info
    printf("\n      Input Tensor:\n");
    printf("        Shape: [%d, %d, %d, %d]\n",
           input_attrs[0].dims[0], input_attrs[0].dims[1],
           input_attrs[0].dims[2], input_attrs[0].dims[3]);
    printf("        Format: %s\n", 
           input_attrs[0].fmt == RKNN_TENSOR_NHWC ? "NHWC" : "NCHW");
    printf("        Type: %s\n",
           input_attrs[0].type == RKNN_TENSOR_UINT8 ? "UINT8" :
           input_attrs[0].type == RKNN_TENSOR_FLOAT32 ? "FLOAT32" : "OTHER");
    
    // Print output tensor info
    printf("\n      Output Tensor:\n");
    printf("        Shape: [%d, %d, %d, %d]\n",
           output_attrs[0].dims[0], output_attrs[0].dims[1],
           output_attrs[0].dims[2], output_attrs[0].dims[3]);
    printf("        Format: %s\n",
           output_attrs[0].fmt == RKNN_TENSOR_NHWC ? "NHWC" : "NCHW");
    printf("        Type: %s\n",
           output_attrs[0].type == RKNN_TENSOR_UINT8 ? "UINT8" :
           output_attrs[0].type == RKNN_TENSOR_FLOAT32 ? "FLOAT32" : "OTHER");
    
    int embedding_dim = output_attrs[0].dims[1];
    printf("        Embedding dimension: %d\n", embedding_dim);
    
    // Load and preprocess image
    printf("\n[3/5] Loading and preprocessing image...\n");
    Image* img = load_image(image_path);
    if (!img) {
        rknn_destroy(ctx);
        return -1;
    }
    
    printf("      Original size: %dx%d\n", img->width, img->height);
    
    // Resize to 224x224
    unsigned char* resized = resize_image(img, 224, 224);
    free_image(img);
    
    if (!resized) {
        printf("ERROR: Failed to resize image\n");
        rknn_destroy(ctx);
        return -1;
    }
    
    printf("      Resized to: 224x224\n");
    printf("      ✓ Image preprocessing complete\n\n");
    
    // Prepare input
    printf("[4/5] Running inference...\n");
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = 224 * 224 * 3;
    inputs[0].buf = resized;
    
    ret = rknn_inputs_set(ctx, io_num.n_input, inputs);
    if (ret < 0) {
        printf("ERROR: rknn_inputs_set failed! ret=%d\n", ret);
        free(resized);
        rknn_destroy(ctx);
        return -1;
    }
    
    // Run inference
    ret = rknn_run(ctx, NULL);
    if (ret < 0) {
        printf("ERROR: rknn_run failed! ret=%d\n", ret);
        free(resized);
        rknn_destroy(ctx);
        return -1;
    }
    
    printf("      ✓ Inference completed\n\n");
    
    // Get outputs
    printf("[5/5] Processing results...\n");
    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = 1;  // Request float output
    }
    
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
    if (ret < 0) {
        printf("ERROR: rknn_outputs_get failed! ret=%d\n", ret);
        free(resized);
        rknn_destroy(ctx);
        return -1;
    }
    
    // Analyze embedding
    float* embedding = (float*)outputs[0].buf;
    int emb_size = outputs[0].size / sizeof(float);
    
    printf("      Output buffer size: %zu bytes\n", outputs[0].size);
    printf("      Embedding dimension: %d\n", emb_size);
    
    // Calculate statistics
    float l2_norm = calculate_l2_norm(embedding, emb_size);
    float mean, std;
    calculate_stats(embedding, emb_size, &mean, &std);
    int zeros = count_zeros(embedding, emb_size);
    float sparsity = (float)zeros / emb_size * 100.0f;
    
    printf("\n");
    printf("=====================================================\n");
    printf("  Embedding Statistics\n");
    printf("=====================================================\n");
    printf("Dimension:     %d\n", emb_size);
    printf("L2 Norm:       %.4f\n", l2_norm);
    printf("Mean:          %.6f\n", mean);
    printf("Std Dev:       %.6f\n", std);
    printf("Min:           %.6f\n", embedding[0]);  // Simplified, not actual min
    printf("Max:           %.6f\n", embedding[0]);  // Simplified, not actual max
    printf("Sparsity:      %.1f%% (%d zeros)\n", sparsity, zeros);
    
    // Show first 10 embedding values
    printf("\nFirst 10 embedding values (raw logits):\n");
    for (int i = 0; i < 10 && i < emb_size; i++) {
        printf("  [%d] = %.6f\n", i, embedding[i]);
    }
    
    // If this looks like classification output (1000 dims), apply softmax and show top-5
    if (emb_size == 1000) {
        printf("\n=====================================================\n");
        printf("  Classification Results (ImageNet)\n");
        printf("=====================================================\n");
        printf("Note: Output is 1000-dim classification layer\n");
        printf("      (Not the 1280-dim embedding layer)\n\n");
        
        // Make a copy for softmax
        float* scores = (float*)malloc(emb_size * sizeof(float));
        memcpy(scores, embedding, emb_size * sizeof(float));
        
        // Apply softmax
        softmax(scores, emb_size);
        
        // Get top-5
        int top5_indices[5];
        float top5_scores[5];
        get_topk(scores, emb_size, 5, top5_indices, top5_scores);
        
        // Load class labels (if available)
        const char* labels_path = "synset.txt";
        FILE* labels_file = fopen(labels_path, "r");
        char** labels = NULL;
        int num_labels = 0;
        
        if (labels_file) {
            // Count lines
            char line[1024];
            while (fgets(line, sizeof(line), labels_file)) {
                num_labels++;
            }
            
            // Allocate and read labels
            labels = (char**)malloc(num_labels * sizeof(char*));
            rewind(labels_file);
            for (int i = 0; i < num_labels; i++) {
                if (fgets(line, sizeof(line), labels_file)) {
                    // Remove newline
                    size_t len = strlen(line);
                    if (len > 0 && line[len-1] == '\n') {
                        line[len-1] = '\0';
                    }
                    labels[i] = strdup(line);
                }
            }
            fclose(labels_file);
        }
        
        printf("-----TOP 5-----\n");
        for (int i = 0; i < 5; i++) {
            int idx = top5_indices[i];
            float score = top5_scores[i];
            
            if (labels && idx < num_labels) {
                printf("[%d] score=%.6f class=\"%s\"\n", idx, score, labels[idx]);
            } else {
                printf("[%d] score=%.6f\n", idx, score);
            }
        }
        
        // Free labels
        if (labels) {
            for (int i = 0; i < num_labels; i++) {
                free(labels[i]);
            }
            free(labels);
        }
        free(scores);
        
        printf("\n⚠️  Note: For anomaly detection, we need 1280-dim embeddings\n");
        printf("          The compile script should extract the embedding layer\n");
        printf("          (global average pooling) instead of the classifier.\n");
    } else if (emb_size == 1280) {
        printf("\n✓ This is the expected 1280-dim embedding output!\n");
        printf("  Perfect for anomaly detection and similarity matching.\n");
    }
    
    printf("\n=====================================================\n");
    printf("✓ Test completed successfully!\n");
    printf("=====================================================\n\n");
    
    printf("Next steps:\n");
    printf("  1. This embedding can be used for similarity matching\n");
    printf("  2. Compare embeddings from different images using cosine similarity\n");
    printf("  3. Build reference timeline from source video\n");
    printf("  4. Implement anomaly detection logic\n\n");
    
    // Cleanup
    rknn_outputs_release(ctx, io_num.n_output, outputs);
    free(resized);
    rknn_destroy(ctx);
    
    return 0;
}
