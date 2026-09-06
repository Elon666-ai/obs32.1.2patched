#pragma once

#include "face-swap-scrfd.h"

/* YuNet (OpenCV Zoo, MIT licence) face detector.
 *
 * Layout differs from SCRFD in three ways that matter here:
 *   - 12 outputs instead of 9: classification and objectness are separate
 *     tensors and the confidence is their product;
 *   - one anchor per cell instead of two;
 *   - boxes and landmarks are offsets relative to the anchor cell, expressed
 *     in stride units, rather than distances to the box edges.
 *
 * Input is raw 0-255 RGB, i.e. no mean subtraction and no scaling, matching
 * OpenCV's FaceDetectorYN.
 */
bool face_swap_yunet_decode(void *param, const struct face_swap_ort_tensor *outputs, size_t output_count);
