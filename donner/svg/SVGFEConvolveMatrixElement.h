#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feConvolveMatrix "<feConvolveMatrix>"
 *
 * `<feConvolveMatrix>` applies a **convolution kernel** to the input image. It's the
 * general-purpose primitive behind blur, sharpen, edge detection, emboss, and countless other
 * image-processing effects. You give it a small grid of numbers (the *kernel*), and each
 * output pixel is computed as a weighted sum of the input pixel and its neighbours using those
 * numbers.
 *
 * If you've used an image editor's "custom filter" or "convolution" dialog, this is the same
 * math. Changing the kernel changes the effect — the same primitive produces wildly different
 * results depending on the numbers you feed it.
 *
 * - DOM object: SVGFEConvolveMatrixElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feConvolveMatrixElement
 *
 * ## How convolution works
 *
 * Imagine sliding the kernel grid over every pixel of the input image. For each position:
 *
 * 1. Multiply each kernel number by the corresponding neighbour pixel's value.
 * 2. Sum all of those products.
 * 3. Divide by `divisor`.
 * 4. Add `bias`.
 * 5. Write the result to the output pixel.
 *
 * That's it. The art is in choosing the kernel: a 3×3 kernel of all 1/9 averages a
 * neighbourhood (blur); a kernel with a large positive centre and small negative neighbours
 * amplifies local differences (sharpen); a kernel that subtracts opposite neighbours highlights
 * intensity gradients (edge detect).
 *
 * ## A critical gotcha: always set `divisor` explicitly
 *
 * If you omit the `divisor` attribute, the default is the **sum of the kernel values**.
 * For a Laplacian-style edge-detect kernel like `0 -1 0 / -1 4 -1 / 0 -1 0`, that sum is
 * `0`, and dividing by zero produces an undefined / broken render (in many implementations
 * the output is simply white or all opaque).
 *
 * **Always set `divisor` explicitly** when your kernel sums to zero, and set a `bias` of
 * `0.5` for edge-detect / emboss kernels so negative results are remapped into the visible
 * [0, 1] range instead of being clamped to black.
 *
 * ## Example 1: box blur
 *
 * All nine cells are `1`, and `divisor="9"` turns the sum back into an average. Each output
 * pixel becomes the average of its 3×3 neighbourhood, producing a mild uniform blur:
 *
 * \htmlonly
 * <svg width="340" height="160" viewBox="0 0 340 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feConv_box">
 *       <feConvolveMatrix order="3" divisor="9" kernelMatrix="1 1 1 1 1 1 1 1 1"/>
 *     </filter>
 *   </defs>
 *   <g transform="translate(10,10)">
 *     <rect width="90" height="90" fill="none" stroke="#888"/>
 *     <g font-size="14" text-anchor="middle" fill="#333">
 *       <text x="15" y="22">1</text><text x="45" y="22">1</text><text x="75" y="22">1</text>
 *       <text x="15" y="52">1</text><text x="45" y="52">1</text><text x="75" y="52">1</text>
 *       <text x="15" y="82">1</text><text x="45" y="82">1</text><text x="75" y="82">1</text>
 *     </g>
 *     <line x1="30" y1="0" x2="30" y2="90" stroke="#ccc"/>
 *     <line x1="60" y1="0" x2="60" y2="90" stroke="#ccc"/>
 *     <line x1="0" y1="30" x2="90" y2="30" stroke="#ccc"/>
 *     <line x1="0" y1="60" x2="90" y2="60" stroke="#ccc"/>
 *     <text x="45" y="108" text-anchor="middle" fill="#444">divisor=9</text>
 *   </g>
 *   <text x="140" y="30" font-size="14" font-weight="bold" fill="steelblue">Text</text>
 *   <text x="140" y="48" fill="#666">original</text>
 *   <text x="220" y="30" font-size="14" font-weight="bold" fill="steelblue" filter="url(#xml_feConv_box)">Text</text>
 *   <text x="220" y="48" fill="#666">box blur</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feConvolveMatrix order="3" divisor="9"
 *                   kernelMatrix="1 1 1
 *                                 1 1 1
 *                                 1 1 1" />
 * ```
 *
 * ## Example 2: sharpen
 *
 * The centre weight is `5` and the four neighbours are `-1`, summing to `1`. Because the
 * centre outweighs the (negative) average of the neighbours, the pixel is pulled further from
 * the local average, exaggerating edges. Set `divisor="1"`:
 *
 * \htmlonly
 * <svg width="340" height="160" viewBox="0 0 340 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feConv_sharp">
 *       <feConvolveMatrix order="3" divisor="1" kernelMatrix="0 -1 0 -1 5 -1 0 -1 0"/>
 *     </filter>
 *   </defs>
 *   <g transform="translate(10,10)">
 *     <rect width="90" height="90" fill="none" stroke="#888"/>
 *     <g font-size="14" text-anchor="middle" fill="#333">
 *       <text x="15" y="22">0</text><text x="45" y="22">-1</text><text x="75" y="22">0</text>
 *       <text x="15" y="52">-1</text><text x="45" y="52">5</text><text x="75" y="52">-1</text>
 *       <text x="15" y="82">0</text><text x="45" y="82">-1</text><text x="75" y="82">0</text>
 *     </g>
 *     <line x1="30" y1="0" x2="30" y2="90" stroke="#ccc"/>
 *     <line x1="60" y1="0" x2="60" y2="90" stroke="#ccc"/>
 *     <line x1="0" y1="30" x2="90" y2="30" stroke="#ccc"/>
 *     <line x1="0" y1="60" x2="90" y2="60" stroke="#ccc"/>
 *     <text x="45" y="108" text-anchor="middle" fill="#444">divisor=1</text>
 *   </g>
 *   <text x="140" y="30" font-size="14" font-weight="bold" fill="steelblue">Text</text>
 *   <text x="140" y="48" fill="#666">original</text>
 *   <text x="220" y="30" font-size="14" font-weight="bold" fill="steelblue" filter="url(#xml_feConv_sharp)">Text</text>
 *   <text x="220" y="48" fill="#666">sharpened</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feConvolveMatrix order="3" divisor="1"
 *                   kernelMatrix="0 -1 0
 *                                -1  5 -1
 *                                 0 -1 0" />
 * ```
 *
 * ## Example 3: edge detect (Laplacian)
 *
 * The kernel sums to `0`, so flat regions produce zero and only rapid changes in intensity
 * (edges) produce non-zero output. Note that we set `divisor="1"` explicitly (the default
 * would be `0` → division by zero → broken render) and `bias="0.5"` to re-centre negative
 * responses into the visible range:
 *
 * \htmlonly
 * <svg width="340" height="160" viewBox="0 0 340 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feConv_edge">
 *       <feConvolveMatrix order="3" divisor="1" bias="0.5" kernelMatrix="0 -1 0 -1 4 -1 0 -1 0"/>
 *     </filter>
 *   </defs>
 *   <g transform="translate(10,10)">
 *     <rect width="90" height="90" fill="none" stroke="#888"/>
 *     <g font-size="14" text-anchor="middle" fill="#333">
 *       <text x="15" y="22">0</text><text x="45" y="22">-1</text><text x="75" y="22">0</text>
 *       <text x="15" y="52">-1</text><text x="45" y="52">4</text><text x="75" y="52">-1</text>
 *       <text x="15" y="82">0</text><text x="45" y="82">-1</text><text x="75" y="82">0</text>
 *     </g>
 *     <line x1="30" y1="0" x2="30" y2="90" stroke="#ccc"/>
 *     <line x1="60" y1="0" x2="60" y2="90" stroke="#ccc"/>
 *     <line x1="0" y1="30" x2="90" y2="30" stroke="#ccc"/>
 *     <line x1="0" y1="60" x2="90" y2="60" stroke="#ccc"/>
 *     <text x="45" y="108" text-anchor="middle" fill="#444">divisor=1, bias=0.5</text>
 *   </g>
 *   <text x="140" y="30" font-size="14" font-weight="bold" fill="steelblue">Text</text>
 *   <text x="140" y="48" fill="#666">original</text>
 *   <text x="220" y="30" font-size="14" font-weight="bold" fill="steelblue" filter="url(#xml_feConv_edge)">Text</text>
 *   <text x="220" y="48" fill="#666">edges</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feConvolveMatrix order="3" divisor="1" bias="0.5"
 *                   kernelMatrix="0 -1 0
 *                                -1  4 -1
 *                                 0 -1 0" />
 * ```
 *
 * ## Example 4: emboss
 *
 * An asymmetric kernel lights one diagonal and darkens the other, simulating a raised surface
 * lit from the upper-left. Again `divisor="1"` and `bias="0.5"` are required for correct
 * output because the kernel sums to zero:
 *
 * \htmlonly
 * <svg width="340" height="160" viewBox="0 0 340 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feConv_emboss">
 *       <feConvolveMatrix order="3" divisor="1" bias="0.5" kernelMatrix="-2 -1 0 -1 1 1 0 1 2"/>
 *     </filter>
 *   </defs>
 *   <g transform="translate(10,10)">
 *     <rect width="90" height="90" fill="none" stroke="#888"/>
 *     <g font-size="14" text-anchor="middle" fill="#333">
 *       <text x="15" y="22">-2</text><text x="45" y="22">-1</text><text x="75" y="22">0</text>
 *       <text x="15" y="52">-1</text><text x="45" y="52">1</text><text x="75" y="52">1</text>
 *       <text x="15" y="82">0</text><text x="45" y="82">1</text><text x="75" y="82">2</text>
 *     </g>
 *     <line x1="30" y1="0" x2="30" y2="90" stroke="#ccc"/>
 *     <line x1="60" y1="0" x2="60" y2="90" stroke="#ccc"/>
 *     <line x1="0" y1="30" x2="90" y2="30" stroke="#ccc"/>
 *     <line x1="0" y1="60" x2="90" y2="60" stroke="#ccc"/>
 *     <text x="45" y="108" text-anchor="middle" fill="#444">divisor=1, bias=0.5</text>
 *   </g>
 *   <text x="140" y="30" font-size="14" font-weight="bold" fill="steelblue">Text</text>
 *   <text x="140" y="48" fill="#666">original</text>
 *   <text x="220" y="30" font-size="14" font-weight="bold" fill="steelblue" filter="url(#xml_feConv_emboss)">Text</text>
 *   <text x="220" y="48" fill="#666">embossed</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <feConvolveMatrix order="3" divisor="1" bias="0.5"
 *                   kernelMatrix="-2 -1 0
 *                                 -1  1 1
 *                                  0  1 2" />
 * ```
 *
 * ## Attributes
 *
 * | Attribute       | Default                  | Description |
 * | --------------: | :----------------------: | :---------- |
 * | `order`         | `3`                      | Size of the kernel matrix. One or two integers (`N` or `cols rows`). A `3` means a 3×3 kernel. |
 * | `kernelMatrix`  | (required)               | `order.x * order.y` numbers, row-major. Whitespace- or comma-separated. |
 * | `divisor`       | sum of kernel values     | Final sum is divided by this. **Always set explicitly** if the kernel sums to zero, otherwise you get a division by zero. |
 * | `bias`          | `0`                      | Added to the result after division. Use `0.5` for edge-detect / emboss kernels so negative responses map into the visible range. |
 * | `targetX`       | `floor(order.x / 2)`     | Which column of the kernel aligns with the output pixel. |
 * | `targetY`       | `floor(order.y / 2)`     | Which row of the kernel aligns with the output pixel. |
 * | `edgeMode`      | `duplicate`              | How pixels outside the input are sampled: `duplicate` (extend edge pixels), `wrap` (tile), or `none` (treat as transparent black). |
 * | `kernelUnitLength` | `1 1`                 | Physical size of one kernel cell in the user coordinate system. Allows resolution-independent kernels. |
 * | `preserveAlpha` | `false`                  | If `true`, the alpha channel is copied through unchanged and convolution only affects RGB. |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feConvolveMatrix element.
 *
 * Applies a matrix convolution filter to the input image, enabling effects such as blurring,
 * edge detection, sharpening, embossing, and beveling.
 */
class SVGFEConvolveMatrixElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEConvolveMatrixElement wrapper from an entity.
  explicit SVGFEConvolveMatrixElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEConvolveMatrixElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeConvolveMatrix;
  /// XML tag name, \ref xml_feConvolveMatrix.
  static constexpr std::string_view Tag{"feConvolveMatrix"};
};

}  // namespace donner::svg
