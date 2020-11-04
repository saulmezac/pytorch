#include <ATen/native/vulkan/ops/Common.h>
#include <ATen/native/ConvUtils.h>
#include <ATen/native/utils/ParamUtils.h>
#include <ATen/native/vulkan/ops/Persistent.h>
#include <torch/custom_class.h>

namespace at {
namespace native {
namespace vulkan {
namespace ops {
namespace {

class Context final : public torch::jit::CustomClassHolder {
 public:
  static Context create(
      const Tensor& weight,
      const c10::optional<Tensor>& bias,
      IntArrayRef stride,
      IntArrayRef padding,
      IntArrayRef dilation,
      bool transposed,
      IntArrayRef output_padding,
      int64_t groups,
      c10::optional<Scalar> output_min = c10::nullopt,
      c10::optional<Scalar> output_max = c10::nullopt);

  using State = std::tuple<
      Tensor,
      c10::optional<Tensor>,
      std::vector<int64_t>,
      std::vector<int64_t>,
      std::vector<int64_t>,
      int64_t,
      c10::optional<Scalar>,
      c10::optional<Scalar>>;

  Tensor run(const Tensor& input) const;
  State unpack() const;

 private:
  Context(
      const Tensor& weight,
      const c10::optional<Tensor>& bias,
      IntArrayRef stride,
      IntArrayRef padding,
      IntArrayRef dilation,
      bool transposed,
      IntArrayRef output_padding,
      int64_t groups,
      c10::optional<Scalar> output_min = c10::nullopt,
      c10::optional<Scalar> output_max = c10::nullopt);

 private:
  struct {
    vTensor v_weight;
    vTensor v_bias;
    std::array<int32_t, 2> stride;
    std::array<int32_t, 2> padding;
    std::array<int32_t, 2> dilation;
    int32_t groups;
    float output_min;
    float output_max;
  } packed_;

  struct {
    Tensor weight;
    c10::optional<Tensor> bias;
    std::vector<int64_t> stride;
    std::vector<int64_t> padding;
    std::vector<int64_t> dilation;
    int64_t groups;
    c10::optional<Scalar> output_min;
    c10::optional<Scalar> output_max;
  } unpacked_;
};

vTensor pack_weights(
    const Tensor& weight,
    const int64_t groups) {
  // Depthwise
  if ((weight.size(Layout::Filter::input) == groups) &&
      (weight.size(Layout::Filter::output) == groups)) {
    if (weight.is_vulkan()) {
      // Assume the weights are already in the right layout.
      return convert(weight);
    }

    vTensor v_weight{
      api::context(),
      &persistent()->pool,
      weight.sizes(),
      weight.options(),
    };

    {
      using Future = vTensor::Future<void, vTensor::Access::Write>;
      Future v_weight_future = v_weight.host<void, vTensor::Access::Write>();
      Future::Payload v_weight_payload = v_weight_future.wait();

      memcpy(
          v_weight_payload.get(),
          weight.contiguous().data_ptr<float>(),
          std::min(weight.nbytes(), v_weight.nbytes()));
    }

    return v_weight;
  }

  TORCH_CHECK(false, "Not implemented!");
}

vTensor pack_biases(
    const c10::optional<Tensor>& bias,
    const Tensor& weight) {
  if (bias && bias->is_vulkan()) {
    TORCH_CHECK(
      weight.size(Layout::Filter::output) == bias->size(Layout::Filter::output),
      "Unexpected bias shape!");

    return convert(*bias);
  }

  vTensor v_bias{
    api::context(),
    &persistent()->pool,
    {
      // 1D
      weight.size(Layout::Filter::output),
    },
    weight.options(),
  };

  {
      using Future = vTensor::Future<void, vTensor::Access::Write>;
      Future v_bias_future = v_bias.host<void, vTensor::Access::Write>();
      Future::Payload v_bias_payload = v_bias_future.wait();

      if (bias) {
        memcpy(
            v_bias_payload.get(),
            bias->contiguous().data_ptr<float>(),
            std::min(bias->nbytes(), v_bias.nbytes()));
      }
      else {
        memset(
            v_bias_payload.get(),
            0,  // Integer and IEEE-754 FP32 have identical 0 bit pattern.
            v_bias.nbytes());
      }
    }

  return v_bias;
}

std::array<int32_t, 2> pack_params(const std::vector<int64_t>& vector) {
  TORCH_INTERNAL_ASSERT(2u == vector.size(), "Invalid usage!");

  return std::array<int32_t, 2>{
    api::utils::safe_downcast<int32_t>(vector[0]),
    api::utils::safe_downcast<int32_t>(vector[1]),
  };
}

Context::Context(
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const IntArrayRef stride,
    const IntArrayRef padding,
    const IntArrayRef dilation,
    const bool /* transposed */,
    const IntArrayRef /* output_padding */,
    const int64_t groups,
    const c10::optional<Scalar> output_min,
    const c10::optional<Scalar> output_max)
  : packed_{
      pack_weights(weight, groups),
      pack_biases(bias, weight),
      pack_params(expand_param_if_needed(stride, "stride", 2)),
      pack_params(expand_param_if_needed(padding, "padding", 2)),
      pack_params(expand_param_if_needed(dilation, "dilation", 2)),
      groups,
      output_min ? output_min->template to<float>() : -std::numeric_limits<float>::infinity(),
      output_max ? output_max->template to<float>() : +std::numeric_limits<float>::infinity(),
    },
    unpacked_{
      weight,
      bias,
      stride.vec(),
      padding.vec(),
      dilation.vec(),
      groups,
      output_min,
      output_max,
    } {
}

bool available(
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const IntArrayRef stride,
    const IntArrayRef padding,
    const IntArrayRef dilation,
    const bool transposed,
    const IntArrayRef /* output_padding */,
    const int64_t groups,
    const c10::optional<Scalar> output_min,
    const c10::optional<Scalar> output_max) {
  return api::available() &&
         // Weight
         (4 == weight.ndimension()) &&
         (weight.size(Layout::Filter::height) > 0) &&
         (weight.size(Layout::Filter::width) > 0) &&
         (c10::DeviceType::Vulkan == weight.device().type()) &&
         (kFloat == weight.scalar_type()) &&
         // Bias
         ((bias && bias->defined()) ? ((1 == bias->ndimension()) &&
                                       (c10::DeviceType::Vulkan == bias->device().type()) &&
                                       (kFloat == bias->scalar_type()) &&
                                       (transposed ? false /* to be addded in the future */
                                                   : (weight.size(Layout::Filter::output) == bias->size(Layout::Filter::output))))
                                    : true) &&
         // Stride
         (stride[Layout::Parameter::height] > 0) &&
         (stride[Layout::Parameter::width] > 0) &&
         // Padding
         (padding[Layout::Parameter::height] >= 0) &&
         (padding[Layout::Parameter::width] >= 0) &&
         // Dilation
         (dilation[Layout::Parameter::height] > 0) &&
         (dilation[Layout::Parameter::width] > 0) &&
         // Groups
         (groups > 0) &&
         // Input
         (weight.size(Layout::Filter::input) > 0) &&
         // Output
         (weight.size(Layout::Filter::output) > 0) &&
         // Output - Groups
         ((weight.size(Layout::Filter::output) % groups) == 0) &&
         // Output Min / Max
         (!(output_min && output_max) ||
          (output_min->isFloatingPoint() && output_max->isFloatingPoint() &&
            (output_max->to<float>() > output_min->to<float>()))) &&
         true;
}

Context Context::create(
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const IntArrayRef stride_arg,
    const IntArrayRef padding_arg,
    const IntArrayRef dilation_arg,
    const bool transposed,
    const IntArrayRef output_padding_arg,
    const int64_t groups,
    const c10::optional<Scalar> output_min,
    const c10::optional<Scalar> output_max) {
  const auto stride = expand_param_if_needed(stride_arg, "stride", 2);
  const auto dilation = expand_param_if_needed(dilation_arg, "dilation", 2);
  const auto padding = expand_param_if_needed(padding_arg, "padding", 2);
  const auto output_padding = expand_param_if_needed(output_padding_arg, "output_padding", 2);

  TORCH_CHECK(
      available(
          weight,
          bias,
          stride,
          padding,
          dilation,
          transposed,
          output_padding,
          groups,
          output_min,
          output_max),
      "Vulkan::convolution not available! "
      "Reason: The provided (weight, bias, stride, padding, dilation, groups, "
      "transposed, output_padding, output_min, output_max) parameters are either "
      "invalid individually or their combination is not supported by Vulkan impl.");

  // Pass in the originals
  return Context{
    weight,
    bias,
    stride_arg,
    padding_arg,
    dilation_arg,
    transposed,
    output_padding_arg,
    groups,
    output_min,
    output_max,
  };
}

bool usable(const Tensor& input) {
         // Input
  return (4 == input.ndimension()) &&
         (c10::DeviceType::Vulkan == input.device().type()) &&
         (kFloat == input.scalar_type()) &&
         (input.size(Layout::Activation4D::batch) >= 0) &&
         (input.size(Layout::Activation4D::channels) > 0) &&
         (input.size(Layout::Activation4D::height) > 0) &&
         (input.size(Layout::Activation4D::width) > 0) &&
         !input.requires_grad() &&
         true;
}

Tensor Context::run(const Tensor& input_arg) const {
  api::Context* const context = api::context();

  const Tensor input = input_arg.is_vulkan() ? input_arg : input_arg.vulkan();
  const vTensor& v_input = convert(input);

  TORCH_CHECK(
      usable(input),
      "Vulkan Convolution not usable! "
      "Reason: The provided input tensor is either invalid or unsupported by Vulkan impl.");

  const IntArrayRef input_sizes = ;
  const IntArrayRef weight_sizes = ;

  vTensor v_output{
    context,
    conv_output_size(
        v_input.sizes(),
        packed_.v_weight.sizes(),
        {
          packed_.padding[0],
          packed_.padding[1],
        },
        {
          packed_.stride[0],
          packed_.stride[1],
        },
        {
          packed_.dilation[0],
          packed_.dilation[1],
        }),
    input.options(),
  };

  api::Command::Buffer command_buffer = context->command().pool.allocate();
  command_buffer.begin();
  {
    if (v_output.has_image() && v_input.has_image() && packed_.v_weight.has_image()) {
      // const struct {
      // } block {
      // };

      context->dispatch(
          command_buffer,
          {
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          },
          VK_KERNEL(conv2d_dw_clamp),
          v_output.extents(),
          // Write-only access bypasses synchronization but inserts appropriate
          // barriers if necessary.
          v_output.image(command_buffer, vTensor::Access::Write),
          // Read-only access is implied on const tensors and triggers an async
          // synchronization if necessary.
          v_input.image(command_buffer),
          // Read-only access is implied on const tensors and triggers an async
          // synchronization if necessary.
          packed_.v_weight.image(command_buffer),
          // Read-only access is implied on const tensors and triggers an async
          // synchronization if necessary.
          packed_.v_bias.buffer(command_buffer),
          // Object lifetime is managed by the resource pool.
          // It is OK not to keep track of the handle.
          context->resource().pool.uniform(block).object);
    }
    else {
      TORCH_CHECK(false, "Not implemented!");
    }
  }
  command_buffer.end();
  command_buffer.submit(context->gpu().queue);

  return convert(v_output);
}

Context::State Context::unpack() const {
  return Context::State{
    unpacked_.weight,
    unpacked_.bias,
    unpacked_.stride,
    unpacked_.padding,
    unpacked_.dilation,
    unpacked_.groups,
    unpacked_.output_min,
    unpacked_.output_max,
  };
}

c10::intrusive_ptr<Context> conv2_clamp_prepack(
    Tensor&& weight,
    c10::optional<Tensor>&& bias,
    std::vector<int64_t>&& stride,
    std::vector<int64_t>&& padding,
    std::vector<int64_t>&& dilation,
    const int64_t groups,
    const c10::optional<Scalar> output_min,
    const c10::optional<Scalar> output_max) {
  return c10::make_intrusive<Context>(
      Context::create(
          std::move(weight),
          std::move(bias),
          std::move(stride),
          std::move(padding),
          std::move(dilation),
          groups,
          /* transposed = */ false,
          /* output_padding = */ {},
          output_min,
          output_min));
}

Tensor conv2d_clamp_run(
    const Tensor& input,
    const c10::intrusive_ptr<Context>& context) {
  return context->run(input);
}

Tensor convolution(
    const Tensor& input,
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const IntArrayRef stride,
    const IntArrayRef padding,
    const IntArrayRef dilation,
    const bool transposed,
    const IntArrayRef output_padding,
    const int64_t groups) {
  return Context::create(
      weight,
      bias,
      stride,
      padding,
      dilation,
      groups,
      /* transposed = */ false,
      /* output_padding = */ {}
  ).run(input);
}

TORCH_LIBRARY(vulkan, m) {
  m.class_<Context>("Conv2dOpContext")
      .def_pickle(
          // __getstate__
          [](const c10::intrusive_ptr<Context>& context) {
            return context->unpack();
          },
          // __setstate__
          [](Context::State state) {
            return conv2_clamp_prepack(
                std::move(std::get<0>(state)),
                std::move(std::get<1>(state)),
                std::move(std::get<2>(state)),
                std::move(std::get<3>(state)),
                std::move(std::get<4>(state)),
                std::move(std::get<5>(state)),
                std::move(std::get<6>(state)),
                std::move(std::get<7>(state)));
          });
}

TORCH_LIBRARY(vulkan_prepack, m) {
  m.def(
      "conv2d_clamp_prepack(Tensor W, Tensor? B, int[2] stride, "
      "int[2] padding, int[2] dilation, int groups, "
      "Scalar? output_min=None, Scalar? output_max=None) "
      "-> __torch__.torch.classes.vulkan.Conv2dOpContext");
  m.def(
      "conv2d_clamp_run(Tensor X, "
      "__torch__.torch.classes.vulkan.Conv2dOpContext W_prepack) -> Tensor Y");
}

TORCH_LIBRARY_IMPL(vulkan_prepack, CPU, m) {
  m.impl("conv2d_clamp_prepack", TORCH_FN(conv2_clamp_prepack));
}

TORCH_LIBRARY_IMPL(vulkan_prepack, Vulkan, m) {
  m.impl("conv2d_clamp_run", conv2d_clamp_run);
}

TORCH_LIBRARY_IMPL(aten, Vulkan, m) {
  m.impl_UNBOXED("convolution_overrideable", convolution);
}

} // namespace
} // namespace ops
} // namespace vulkan
} // namespace native
} // namespace at
