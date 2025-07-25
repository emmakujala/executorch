/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <executorch/examples/models/llama/runner/runner.h>
#include <executorch/examples/models/llava/runner/llava_runner.h>
#include <executorch/extension/llm/runner/image.h>
#include <executorch/extension/llm/runner/irunner.h>
#include <executorch/runtime/platform/log.h>
#include <executorch/runtime/platform/platform.h>
#include <executorch/runtime/platform/runtime.h>

#if defined(ET_USE_THREADPOOL)
#include <executorch/extension/threadpool/cpuinfo_utils.h>
#include <executorch/extension/threadpool/threadpool.h>
#endif

#include <fbjni/ByteBuffer.h>
#include <fbjni/fbjni.h>

#if defined(EXECUTORCH_BUILD_MEDIATEK)
#include <executorch/examples/mediatek/executor_runner/mtk_llama_runner.h>
#endif

namespace llm = ::executorch::extension::llm;
using ::executorch::runtime::Error;

namespace {
bool utf8_check_validity(const char* str, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = static_cast<uint8_t>(str[i]);
    if (byte >= 0x80) { // Non-ASCII byte
      if (i + 1 >= length) { // Incomplete sequence
        return false;
      }
      uint8_t next_byte = static_cast<uint8_t>(str[i + 1]);
      if ((byte & 0xE0) == 0xC0 &&
          (next_byte & 0xC0) == 0x80) { // 2-byte sequence
        i += 1;
      } else if (
          (byte & 0xF0) == 0xE0 && (next_byte & 0xC0) == 0x80 &&
          (i + 2 < length) &&
          (static_cast<uint8_t>(str[i + 2]) & 0xC0) ==
              0x80) { // 3-byte sequence
        i += 2;
      } else if (
          (byte & 0xF8) == 0xF0 && (next_byte & 0xC0) == 0x80 &&
          (i + 2 < length) &&
          (static_cast<uint8_t>(str[i + 2]) & 0xC0) == 0x80 &&
          (i + 3 < length) &&
          (static_cast<uint8_t>(str[i + 3]) & 0xC0) ==
              0x80) { // 4-byte sequence
        i += 3;
      } else {
        return false; // Invalid sequence
      }
    }
  }
  return true; // All bytes were valid
}

std::string token_buffer;
} // namespace

namespace executorch_jni {

class ExecuTorchLlmCallbackJni
    : public facebook::jni::JavaClass<ExecuTorchLlmCallbackJni> {
 public:
  constexpr static const char* kJavaDescriptor =
      "Lorg/pytorch/executorch/extension/llm/LlmCallback;";

  void onResult(std::string result) const {
    static auto cls = ExecuTorchLlmCallbackJni::javaClassStatic();
    static const auto method =
        cls->getMethod<void(facebook::jni::local_ref<jstring>)>("onResult");

    token_buffer += result;
    if (!utf8_check_validity(token_buffer.c_str(), token_buffer.size())) {
      ET_LOG(
          Info, "Current token buffer is not valid UTF-8. Waiting for more.");
      return;
    }
    result = token_buffer;
    token_buffer = "";
    facebook::jni::local_ref<jstring> s = facebook::jni::make_jstring(result);
    method(self(), s);
  }

  void onStats(const llm::Stats& result) const {
    static auto cls = ExecuTorchLlmCallbackJni::javaClassStatic();
    static const auto on_stats_method =
        cls->getMethod<void(facebook::jni::local_ref<jstring>)>("onStats");
    on_stats_method(
        self(),
        facebook::jni::make_jstring(
            executorch::extension::llm::stats_to_json_string(result)));
  }
};

class ExecuTorchLlmJni : public facebook::jni::HybridClass<ExecuTorchLlmJni> {
 private:
  friend HybridBase;
  float temperature_ = 0.0f;
  int model_type_category_;
  std::unique_ptr<llm::IRunner> runner_;
  std::unique_ptr<llm::MultimodalRunner> multi_modal_runner_;

 public:
  constexpr static auto kJavaDescriptor =
      "Lorg/pytorch/executorch/extension/llm/LlmModule;";

  constexpr static int MODEL_TYPE_CATEGORY_LLM = 1;
  constexpr static int MODEL_TYPE_CATEGORY_MULTIMODAL = 2;
  constexpr static int MODEL_TYPE_MEDIATEK_LLAMA = 3;

  static facebook::jni::local_ref<jhybriddata> initHybrid(
      facebook::jni::alias_ref<jclass>,
      jint model_type_category,
      facebook::jni::alias_ref<jstring> model_path,
      facebook::jni::alias_ref<jstring> tokenizer_path,
      jfloat temperature,
      facebook::jni::alias_ref<jstring> data_path) {
    return makeCxxInstance(
        model_type_category,
        model_path,
        tokenizer_path,
        temperature,
        data_path);
  }

  ExecuTorchLlmJni(
      jint model_type_category,
      facebook::jni::alias_ref<jstring> model_path,
      facebook::jni::alias_ref<jstring> tokenizer_path,
      jfloat temperature,
      facebook::jni::alias_ref<jstring> data_path = nullptr) {
    temperature_ = temperature;
#if defined(ET_USE_THREADPOOL)
    // Reserve 1 thread for the main thread.
    int32_t num_performant_cores =
        ::executorch::extension::cpuinfo::get_num_performant_cores() - 1;
    if (num_performant_cores > 0) {
      ET_LOG(Info, "Resetting threadpool to %d threads", num_performant_cores);
      ::executorch::extension::threadpool::get_threadpool()
          ->_unsafe_reset_threadpool(num_performant_cores);
    }
#endif

    model_type_category_ = model_type_category;
    if (model_type_category == MODEL_TYPE_CATEGORY_MULTIMODAL) {
      multi_modal_runner_ = std::make_unique<example::LlavaRunner>(
          model_path->toStdString().c_str(),
          tokenizer_path->toStdString().c_str(),
          temperature);
    } else if (model_type_category == MODEL_TYPE_CATEGORY_LLM) {
      std::optional<const std::string> data_path_str = data_path
          ? std::optional<const std::string>{data_path->toStdString()}
          : std::nullopt;
      // TODO(larryliu0820): Use the API in text_llm_runner.h to create the
      // runner.
      runner_ = example::create_llama_runner(
          model_path->toStdString(),
          tokenizer_path->toStdString(),
          data_path_str);
#if defined(EXECUTORCH_BUILD_MEDIATEK)
    } else if (model_type_category == MODEL_TYPE_MEDIATEK_LLAMA) {
      runner_ = std::make_unique<MTKLlamaRunner>(
          model_path->toStdString().c_str(),
          tokenizer_path->toStdString().c_str());
      // Interpret the model type as LLM
      model_type_category_ = MODEL_TYPE_CATEGORY_LLM;
#endif
    }
  }

  jint generate(
      facebook::jni::alias_ref<jintArray> image,
      jint width,
      jint height,
      jint channels,
      facebook::jni::alias_ref<jstring> prompt,
      jint seq_len,
      facebook::jni::alias_ref<ExecuTorchLlmCallbackJni> callback,
      jboolean echo) {
    if (model_type_category_ == MODEL_TYPE_CATEGORY_MULTIMODAL) {
      auto image_size = image->size();
      std::vector<llm::Image> images;
      if (image_size != 0) {
        std::vector<jint> image_data_jint(image_size);
        std::vector<uint8_t> image_data(image_size);
        image->getRegion(0, image_size, image_data_jint.data());
        for (int i = 0; i < image_size; i++) {
          image_data[i] = image_data_jint[i];
        }
        llm::Image image_runner{image_data, width, height, channels};
        images.push_back(image_runner);
      }
      multi_modal_runner_->generate(
          std::move(images),
          prompt->toStdString(),
          seq_len,
          [callback](std::string result) { callback->onResult(result); },
          [callback](const llm::Stats& result) { callback->onStats(result); },
          echo);
    } else if (model_type_category_ == MODEL_TYPE_CATEGORY_LLM) {
      executorch::extension::llm::GenerationConfig config{
          .echo = static_cast<bool>(echo),
          .seq_len = seq_len,
          .temperature = temperature_,
      };
      runner_->generate(
          prompt->toStdString(),
          config,
          [callback](std::string result) { callback->onResult(result); },
          [callback](const llm::Stats& result) { callback->onStats(result); });
    }
    return 0;
  }

  // Returns a tuple of (error, start_pos)
  // Contract is valid within an AAR (JNI + corresponding Java code)
  // If the first element is not Error::Ok, the other element is undefined.
  facebook::jni::local_ref<jlongArray> prefill_prompt(
      facebook::jni::alias_ref<jstring> prompt,
      jlong start_pos,
      jint bos,
      jint eos) {
    facebook::jni::local_ref<jlongArray> tuple_result =
        facebook::jni::make_long_array(2);
    if (model_type_category_ != MODEL_TYPE_CATEGORY_MULTIMODAL) {
      tuple_result->pin()[0] = static_cast<jint>(Error::NotSupported);
      return tuple_result;
    }

    auto&& result = multi_modal_runner_->prefill_prompt(
        prompt->toStdString(), start_pos, bos, eos);
    tuple_result->pin()[0] = static_cast<jint>(Error::Ok);
    if (result.ok()) {
      tuple_result->pin()[1] = static_cast<jlong>(start_pos);
    }
    return tuple_result;
  }

  // Returns a tuple of (error, start_pos)
  // Contract is valid within an AAR (JNI + corresponding Java code)
  // If the first element is not Error::Ok, the other element is undefined.

  facebook::jni::local_ref<jlongArray> prefill_images(
      facebook::jni::alias_ref<jintArray> image,
      jint width,
      jint height,
      jint channels,
      jlong start_pos) {
    facebook::jni::local_ref<jlongArray> tuple_result =
        facebook::jni::make_long_array(2);

    if (model_type_category_ != MODEL_TYPE_CATEGORY_MULTIMODAL) {
      tuple_result->pin()[0] = static_cast<jint>(Error::NotSupported);
      return tuple_result;
    }

    auto image_size = image->size();
    std::vector<llm::Image> images;
    if (image_size != 0) {
      std::vector<jint> image_data_jint(image_size);
      std::vector<uint8_t> image_data(image_size);
      image->getRegion(0, image_size, image_data_jint.data());
      for (int i = 0; i < image_size; i++) {
        image_data[i] = image_data_jint[i];
      }
      llm::Image image_runner{image_data, width, height, channels};
      images.push_back(image_runner);
    }
    // TODO(hsz): make  start_pos a reference and update it here
    jint result = static_cast<jint>(
        multi_modal_runner_->prefill_images(images, start_pos));
    tuple_result->pin()[0] = result;
    tuple_result->pin()[1] = static_cast<jlong>(start_pos);
    return tuple_result;
  }

  jint generate_from_pos(
      facebook::jni::alias_ref<jstring> prompt,
      jint seq_len,
      jlong start_pos,
      facebook::jni::alias_ref<ExecuTorchLlmCallbackJni> callback,
      jboolean echo) {
    if (model_type_category_ == MODEL_TYPE_CATEGORY_MULTIMODAL) {
      return static_cast<jint>(multi_modal_runner_->generate_from_pos(
          prompt->toStdString(),
          seq_len,
          start_pos,
          [callback](const std::string& result) { callback->onResult(result); },
          [callback](const llm::Stats& stats) { callback->onStats(stats); },
          echo));
    } else if (model_type_category_ == MODEL_TYPE_CATEGORY_LLM) {
      executorch::extension::llm::GenerationConfig config{
          .echo = static_cast<bool>(echo),
          .seq_len = seq_len,
          .temperature = temperature_,
      };
      return static_cast<jint>(runner_->generate_from_pos(
          prompt->toStdString(),
          start_pos,
          config,
          [callback](std::string result) { callback->onResult(result); },
          [callback](const llm::Stats& stats) { callback->onStats(stats); }));
    }
  }

  void stop() {
    if (model_type_category_ == MODEL_TYPE_CATEGORY_MULTIMODAL) {
      multi_modal_runner_->stop();
    } else if (model_type_category_ == MODEL_TYPE_CATEGORY_LLM) {
      runner_->stop();
    }
  }

  jint load() {
    if (model_type_category_ == MODEL_TYPE_CATEGORY_MULTIMODAL) {
      return static_cast<jint>(multi_modal_runner_->load());
    } else if (model_type_category_ == MODEL_TYPE_CATEGORY_LLM) {
      return static_cast<jint>(runner_->load());
    }
    return static_cast<jint>(Error::InvalidArgument);
  }

  static void registerNatives() {
    registerHybrid({
        makeNativeMethod("initHybrid", ExecuTorchLlmJni::initHybrid),
        makeNativeMethod("generate", ExecuTorchLlmJni::generate),
        makeNativeMethod("stop", ExecuTorchLlmJni::stop),
        makeNativeMethod("load", ExecuTorchLlmJni::load),
        makeNativeMethod(
            "prefillImagesNative", ExecuTorchLlmJni::prefill_images),
        makeNativeMethod(
            "prefillPromptNative", ExecuTorchLlmJni::prefill_prompt),
        makeNativeMethod(
            "generateFromPos", ExecuTorchLlmJni::generate_from_pos),
    });
  }
};

} // namespace executorch_jni

void register_natives_for_llm() {
  executorch_jni::ExecuTorchLlmJni::registerNatives();
}
