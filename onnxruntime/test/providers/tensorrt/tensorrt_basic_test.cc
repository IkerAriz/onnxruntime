// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/graph/onnx_protobuf.h"
#include "core/session/inference_session.h"
#include "test/providers/provider_test_utils.h"
#include "test/framework/test_utils.h"
#include "gtest/gtest.h"
#include "test/util/include/default_providers.h"
#include "test/util/include/scoped_env_vars.h"
#include "core/providers/tensorrt/tensorrt_provider_options.h"
#include <string>
#include <iostream>
#include <filesystem>
namespace fs = std::filesystem;

using namespace std;
using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::logging;

namespace onnxruntime {

namespace test {
class TensorrtExecutionProviderCacheTest: public testing::TestWithParam<std::basic_string<ORTCHAR_T>> {};

template <typename T>
void VerifyOutputs(const std::vector<OrtValue>& fetches, const std::vector<int64_t>& expected_dims,
                   const std::vector<T>& expected_values) {
  ASSERT_EQ(1, fetches.size());
  auto& rtensor = fetches.front().Get<Tensor>();
  TensorShape expected_shape(expected_dims);
  ASSERT_EQ(expected_shape, rtensor.Shape());
  const std::vector<T> found(rtensor.template Data<T>(), rtensor.template Data<T>() + expected_values.size());
  ASSERT_EQ(expected_values, found);
}

bool IsTensorRTCacheExisted(std::basic_string<ORTCHAR_T> path, std::basic_string<ORTCHAR_T> file_extension) {
  for (const auto & entry : fs::directory_iterator(path)) {
      if (file_extension.compare(fs::path(entry).extension()) == 0) {
          return true;
      }
  }
  return false;
}

void RemoveTensorRTCache(std::basic_string<ORTCHAR_T> path, std::basic_string<ORTCHAR_T> file_extension) {
  for (const auto & entry : fs::directory_iterator(path)) {
      if (file_extension.compare(fs::path(entry).extension()) == 0) {
          fs::remove(entry);
      }
  }
}

void CreateBaseModel(std::basic_string<ORTCHAR_T> model_name, std::basic_string<ORTCHAR_T> graph_name, bool is_dynamic_input_shape, std::vector<int> dims) {
  onnxruntime::Model model(graph_name, false, DefaultLoggingManager().DefaultLogger());
  auto& graph = model.MainGraph();
  std::vector<onnxruntime::NodeArg*> inputs;
  std::vector<onnxruntime::NodeArg*> outputs;

  // FLOAT tensor
  ONNX_NAMESPACE::TypeProto float_tensor;
  float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);

  for (auto dim: dims) {
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(dim);
  }

  if (is_dynamic_input_shape) {
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("sym1");
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("sym2");
  }

  auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &float_tensor);
  auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &float_tensor);
  inputs.push_back(&input_arg_1);
  inputs.push_back(&input_arg_2);
  auto& output_arg = graph.GetOrCreateNodeArg("node_1_out_1", &float_tensor);
  outputs.push_back(&output_arg);
  graph.AddNode("node_1", "Add", "node 1.", inputs, outputs);

  auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &float_tensor);
  inputs.clear();
  inputs.push_back(&output_arg);
  inputs.push_back(&input_arg_3);
  auto& output_arg_2 = graph.GetOrCreateNodeArg("M", &float_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_2);
  graph.AddNode("node_2", "Add", "node 2.", inputs, outputs);

  auto status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
  status = onnxruntime::Model::Save(model, model_name);
}

TEST_P(TensorrtExecutionProviderCacheTest, Run) {
  // GetParam() consists of two main parameters:
  // - cache type (engine cache, profile cache and timing cache)
  // - input type (dynamic input shape or static input shape). 
  // Note: it might have other paramters used for specific situation
  std::basic_string<ORTCHAR_T> param = GetParam();
  std::basic_string<ORTCHAR_T> input_type = "static";
  std::basic_string<ORTCHAR_T> engine_info = "enginecache_disable"; // for timigh cache case only
  size_t pos = param.find(ORT_TSTR("_"));
  ASSERT_NE(pos, std::string::npos);
  std::basic_string<ORTCHAR_T> cache_type = ToUTF8String(param.substr(0, pos));
  if (cache_type.compare("timing") == 0) {
    std::basic_string<ORTCHAR_T> suffix = param.substr(pos + 1);
    size_t suffix_pos = suffix.find(ORT_TSTR("_"));
    input_type = ToUTF8String(suffix.substr(0, suffix_pos));
    engine_info = suffix.substr(suffix_pos + 1);
  } else {
    input_type = param.substr(pos + 1);
  }

  std::basic_string<ORTCHAR_T> model_name = "trt_execution_provider_" + cache_type + "caching_test_" + input_type + ".onnx";
  std::vector<int> dims; // static dims
  if (input_type.compare("dynamic") == 0) {
    dims.push_back(1);
    CreateBaseModel(model_name, cache_type + "cachingtest", true, dims); // dynamic input shape
    // dims is (1, sym1, sym2)
  }
  else {
    dims.push_back(1);
    dims.push_back(3);
    dims.push_back(2);
    CreateBaseModel(model_name, cache_type + "cachingtest", false, dims); // non-dynamic input shape 
    // dims is (1, 3, 2)
  }

  SessionOptions so;
  so.session_logid = "TensorrtExecutionProvider" + cache_type + "cacheTest";
  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  InferenceSession session_object{so, GetEnvironment()};
  auto allocator_manager = session_object.GetAllocatorManager();
  auto cuda_provider = DefaultCudaExecutionProvider();
  cuda_provider->RegisterAllocator(allocator_manager);
  auto cpu_allocator = cuda_provider->GetAllocator(0, OrtMemTypeCPU);
  std::vector<int64_t> dims_mul_x = {1, 3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  OrtValue ml_value_x;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_z);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("M");
  std::vector<OrtValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_m = {1, 3, 2};
  std::vector<float> expected_values_mul_m = {3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f};

  OrtTensorRTProviderOptionsV2 params{
      0,
      0,
      nullptr,
      1000,
      1,
      1 << 30,
      0,
      0,
      nullptr,
      0,
      0,
      0,
      0,
      0,
      nullptr,
      0,
      nullptr,
      0,
      0};

  if (cache_type.compare("timing") == 0) {

    // create ort session
    params.trt_timing_cache_enable = 1;
    if (engine_info.compare("enginecache_enable") == 0)
      params.trt_engine_cache_enable = 1;
    std::unique_ptr<IExecutionProvider> execution_provider = TensorrtExecutionProviderWithOptions(&params);
    EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());
    auto status = session_object.Load(model_name);
    ASSERT_TRUE(status.IsOK());
    status = session_object.Initialize();
    ASSERT_TRUE(status.IsOK());

    // run inference 
    // timing cache should be created under the situation of non-dynamic/dynamic shape input and engine cache enabled/disabled 
    status = session_object.Run(run_options, feeds, output_names, &fetches);
    ASSERT_TRUE(status.IsOK());
    VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
    ASSERT_TRUE(IsTensorRTCacheExisted("./", ".timing"));
    RemoveTensorRTCache("./", ".timing");

    // run inference 
    // timing cache shoud not be used or created since input shape is not changed and engine won't be re-built 
    status = session_object.Run(run_options, feeds, output_names, &fetches);
    ASSERT_TRUE(status.IsOK());
    VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
    ASSERT_TRUE(!IsTensorRTCacheExisted("./", ".timing"));

    // create another ort session to test
    InferenceSession session_object_2{so, GetEnvironment()};
    execution_provider = TensorrtExecutionProviderWithOptions(&params);
    EXPECT_TRUE(session_object_2.RegisterExecutionProvider(std::move(execution_provider)).IsOK());
    status = session_object_2.Load(model_name);
    ASSERT_TRUE(status.IsOK());
    status = session_object_2.Initialize();
    ASSERT_TRUE(status.IsOK());

    if (engine_info.compare("enginecache_enable") == 0) {
      // engine cache is enabled
 
      // run inference 
      // timing cache shoud not be created since engine cache is existed and will be used
      status = session_object_2.Run(run_options, feeds, output_names, &fetches);
      ASSERT_TRUE(status.IsOK());
      VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
      ASSERT_TRUE(!IsTensorRTCacheExisted("./", ".timing"));
    } else {
      // engine cache is not enabled

      // run inference 
      // timing cache shoud be created
      status = session_object_2.Run(run_options, feeds, output_names, &fetches);
      ASSERT_TRUE(status.IsOK());
      VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
      ASSERT_TRUE(IsTensorRTCacheExisted("./", ".timing"));
      RemoveTensorRTCache("./", ".timing");
    }

    if (input_type.compare("dynamic") == 0) {
      // dynamic input shape

      // inference run with input shape {1, 1, 6}
      // timing cache will be created
      // TRT engine and profile will be updated
      dims_mul_x = {1, 1, 6};
      CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
      CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_y);
      CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_z);
      feeds.clear();
      feeds.insert(std::make_pair("X", ml_value_x));
      feeds.insert(std::make_pair("Y", ml_value_y));
      feeds.insert(std::make_pair("Z", ml_value_z));

      status = session_object_2.Run(run_options, feeds, output_names, &fetches);
      ASSERT_TRUE(status.IsOK());
      VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
      ASSERT_TRUE(IsTensorRTCacheExisted("./", ".timing"));
    }

    // clean up caches for another session
    RemoveTensorRTCache("./", ".timing");
    RemoveTensorRTCache("./", ".profile");
    RemoveTensorRTCache("./", ".engine");

  } else if (cache_type.compare("engine") == 0) {
    // #TODO
  } else if (cache_type.compare("profile") == 0) {
    // #TODO
  }
}

auto ExpandModelName  = [](const ::testing::TestParamInfo<TensorrtExecutionProviderCacheTest::ParamType>& info) {
  // use info.param here to generate the test suffix
  std::basic_string<ORTCHAR_T> name = info.param;
#ifdef _WIN32
  // Note: The return value of INSTANTIATE_TEST_SUITE_P accpets std::basic_string<char...>.
  // Need conversion of wchar_t to char.
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(name);
#else
  return name;
#endif
};

// timing_dynamic_enginecache_enable: timing cache enabled, dynamic input shape and engine cache enable
// timing_dynamic_enginecache_disable: timing cache enabled, dynamic input shape and engine cache disable
// timing_static_enginecache_enable: timing cache enabled, static input shape and engine cache enable
// timing_static_enginecache_disable: timing cache enabled, static input shape and engine cache disable
INSTANTIATE_TEST_SUITE_P(TensorrtExecutionProviderCacheTests, TensorrtExecutionProviderCacheTest, testing::Values("timing_dynamic_enginecache_enable",
                                                                                                                  "timing_dynamic_enginecache_disable",
                                                                                                                  "timing_static_enginecache_enable",
                                                                                                                  "timing_static_enginecache_disable"),
                                                                                                  ExpandModelName);

TEST(TensorrtExecutionProviderTest, EngineCachingTest) {
  ScopedEnvironmentVariables scoped_env_vars{EnvVarMap{
      {"ORT_TENSORRT_ENGINE_CACHE_ENABLE", {"1"}},
  }};
  onnxruntime::Model model("enginecachingtest", false, DefaultLoggingManager().DefaultLogger());
  auto& graph = model.MainGraph();
  std::vector<onnxruntime::NodeArg*> inputs;
  std::vector<onnxruntime::NodeArg*> outputs;

  // FLOAT tensor
  ONNX_NAMESPACE::TypeProto float_tensor;
  float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("sym1");
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("sym2");

  auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &float_tensor);
  auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &float_tensor);
  inputs.push_back(&input_arg_1);
  inputs.push_back(&input_arg_2);
  auto& output_arg = graph.GetOrCreateNodeArg("node_1_out_1", &float_tensor);
  outputs.push_back(&output_arg);
  graph.AddNode("node_1", "Add", "node 1.", inputs, outputs);

  auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &float_tensor);
  inputs.clear();
  inputs.push_back(&output_arg);
  inputs.push_back(&input_arg_3);
  auto& output_arg_2 = graph.GetOrCreateNodeArg("M", &float_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_2);
  graph.AddNode("node_2", "Add", "node 2.", inputs, outputs);

  auto status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
  std::string model_file_name = "trt_execution_provider_enginecaching_test.onnx";
  status = onnxruntime::Model::Save(model, model_file_name);

  SessionOptions so;
  so.session_logid = "TensorrtExecutionProviderTest.EngineCachingTest";
  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  InferenceSession session_object{so, GetEnvironment()};
  auto allocator_manager = session_object.GetAllocatorManager();
  auto cuda_provider = DefaultCudaExecutionProvider();
  cuda_provider->RegisterAllocator(allocator_manager);
  auto cpu_allocator = cuda_provider->GetAllocator(0, OrtMemTypeCPU);
  // First run with input shape {1, 3, 2}
  // TRT engine and profile will be created and cached
  // Data in profile,
  // X: 1, 3, 3, 2, 2, 2
  // Y: 1, 3, 3, 2, 2, 2
  // Z: 1, 3, 3, 2, 2, 2
  std::vector<int64_t> dims_mul_x = {1, 3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  OrtValue ml_value_x;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_z);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("M");
  std::vector<OrtValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_m = {1, 3, 2};
  std::vector<float> expected_values_mul_m = {3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f};

  std::unique_ptr<IExecutionProvider> execution_provider = DefaultTensorrtExecutionProvider();
  EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());
  status = session_object.Load(model_file_name);
  ASSERT_TRUE(status.IsOK());
  status = session_object.Initialize();
  ASSERT_TRUE(status.IsOK());

  // Now run
  status = session_object.Run(run_options, feeds, output_names, &fetches);
  ASSERT_TRUE(status.IsOK());
  VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);

  // Second run with input shape {1, 1, 6}
  // TRT engine and profile will be updated
  // Data in profile,
  // X: 1, 1, 3, 2, 2, 6
  // Y: 1, 1, 3, 2, 2, 6
  // Z: 1, 1, 3, 2, 2, 6
  dims_mul_x = {1, 1, 6};
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_y);
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_z);
  feeds.clear();
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  fetches.clear();

  // prepare expected inputs and outputs
  expected_dims_mul_m = {1, 1, 6};

  // Now run
  status = session_object.Run(run_options, feeds, output_names, &fetches);
  ASSERT_TRUE(status.IsOK());
  VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
}

TEST(TensorrtExecutionProviderTest, FunctionTest) {
  onnxruntime::Model model("functiontest", false, DefaultLoggingManager().DefaultLogger());
  auto& graph = model.MainGraph();
  std::vector<onnxruntime::NodeArg*> inputs;
  std::vector<onnxruntime::NodeArg*> outputs;

  // FLOAT tensor.
  ONNX_NAMESPACE::TypeProto float_tensor;
  float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &float_tensor);
  auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &float_tensor);
  inputs.push_back(&input_arg_1);
  inputs.push_back(&input_arg_2);
  auto& output_arg = graph.GetOrCreateNodeArg("node_1_out_1", &float_tensor);
  outputs.push_back(&output_arg);
  graph.AddNode("node_1", "Add", "node 1.", inputs, outputs);

  auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &float_tensor);
  inputs.clear();
  inputs.push_back(&output_arg);
  inputs.push_back(&input_arg_3);
  auto& output_arg_2 = graph.GetOrCreateNodeArg("M", &float_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_2);
  graph.AddNode("node_2", "Add", "node 2.", inputs, outputs);

  auto status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
  std::string model_file_name = "trt_execution_provider_function_test.onnx";
  status = onnxruntime::Model::Save(model, model_file_name);

  SessionOptions so;
  so.session_logid = "TensorrtExecutionProviderTest.FunctionTest";
  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  InferenceSession session_object{so, GetEnvironment()};

  auto allocator_manager = session_object.GetAllocatorManager();
  auto cuda_provider = DefaultCudaExecutionProvider();
  cuda_provider->RegisterAllocator(allocator_manager);
  auto cpu_allocator = cuda_provider->GetAllocator(0, OrtMemTypeCPU);

  std::vector<int64_t> dims_mul_x = {1, 3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  OrtValue ml_value_x;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<float>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_z);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("M");
  std::vector<OrtValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_m = {1, 3, 2};
  std::vector<float> expected_values_mul_m = {3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f};

  std::unique_ptr<IExecutionProvider> execution_provider = DefaultTensorrtExecutionProvider();
  EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());

  status = session_object.Load(model_file_name);
  ASSERT_TRUE(status.IsOK());
  status = session_object.Initialize();
  ASSERT_TRUE(status.IsOK());

  // Now run
  status = session_object.Run(run_options, feeds, output_names, &fetches);
  ASSERT_TRUE(status.IsOK());
  VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
}

TEST(TensorrtExecutionProviderTest, NodeIndexMappingTest) {
  onnxruntime::Model model("nodeindexmappingtest", false, DefaultLoggingManager().DefaultLogger());
  auto& graph = model.MainGraph();
  std::vector<onnxruntime::NodeArg*> inputs;
  std::vector<onnxruntime::NodeArg*> outputs;

  // FLOAT tensor.
  ONNX_NAMESPACE::TypeProto float_tensor;
  float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  // BOOL tensor.
  ONNX_NAMESPACE::TypeProto bool_tensor;
  bool_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_BOOL);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  // UINT8 tensor.
  ONNX_NAMESPACE::TypeProto uint8_tensor;
  uint8_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_UINT8);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &bool_tensor);
  inputs.push_back(&input_arg_1);
  auto& output_arg_1 = graph.GetOrCreateNodeArg("node_1_out", &uint8_tensor);
  outputs.push_back(&output_arg_1);
  auto& cast_node = graph.AddNode("cast1", "Cast", "node 1.", inputs, outputs);
  AttributeProto attr_proto;
  attr_proto.set_name("to");
  attr_proto.set_type(AttributeProto_AttributeType_INT);
  attr_proto.set_i(2);
  cast_node.AddAttribute("to", attr_proto);

  inputs.clear();
  inputs.push_back(&output_arg_1);
  auto& output_arg_2 = graph.GetOrCreateNodeArg("M", &bool_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_2);
  auto& cast_node_2 = graph.AddNode("cast2", "Cast", "node 2.", inputs, outputs);
  AttributeProto attr_proto_2;
  attr_proto_2.set_name("to");
  attr_proto_2.set_type(AttributeProto_AttributeType_INT);
  attr_proto_2.set_i(9);
  cast_node_2.AddAttribute("to", attr_proto_2);

  auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &float_tensor);
  auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &float_tensor);
  inputs.clear();
  inputs.push_back(&input_arg_2);
  inputs.push_back(&input_arg_3);
  auto& output_arg_3 = graph.GetOrCreateNodeArg("N", &float_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_3);
  graph.AddNode("sub", "Sub", "node 3.", inputs, outputs);

  auto status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
  std::string model_file_name = "trt_execution_provider_nodeindexmapping_test.onnx";
  status = onnxruntime::Model::Save(model, model_file_name);

  SessionOptions so;
  so.session_logid = "TensorrtExecutionProviderTest.NodeIndexMappingTest";
  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  InferenceSession session_object{so, GetEnvironment()};

  auto allocator_manager = session_object.GetAllocatorManager();
  auto cuda_provider = DefaultCudaExecutionProvider();
  cuda_provider->RegisterAllocator(allocator_manager);
  auto cpu_allocator = cuda_provider->GetAllocator(0, OrtMemTypeCPU);

  std::vector<int64_t> dims_mul_x = {1, 3, 2};
  std::vector<bool> values_mul_x = {true, false, true, false, true, false};
  std::vector<int64_t> dims_mul_y = {1, 3, 2};
  std::vector<float> values_mul_y = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  OrtValue ml_value_x;
  CreateMLValue<bool>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<float>(cpu_allocator, dims_mul_y, values_mul_y, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<float>(cpu_allocator, dims_mul_y, values_mul_y, &ml_value_z);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("M");
  output_names.push_back("N");
  std::vector<OrtValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_m = {1, 3, 2};
  std::vector<bool> expected_values_mul_m = {true, false, true, false, true, false};
  std::vector<int64_t> expected_dims_mul_n = {1, 3, 2};
  std::vector<float> expected_values_mul_n = {0, 0, 0, 0, 0, 0};

  std::unique_ptr<IExecutionProvider> execution_provider = DefaultTensorrtExecutionProvider();
  EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());

  ASSERT_STATUS_OK(session_object.Load(model_file_name));
  ASSERT_STATUS_OK(session_object.Initialize());

  // Now run
  ASSERT_STATUS_OK(session_object.Run(run_options, feeds, output_names, &fetches));
  std::vector<OrtValue> fetche{fetches.back()};
  VerifyOutputs(fetche, expected_dims_mul_n, expected_values_mul_n);
}

TEST(TensorrtExecutionProviderTest, RemoveCycleTest) {
  onnxruntime::Model model("removecycletest", false, DefaultLoggingManager().DefaultLogger());
  auto& graph = model.MainGraph();
  std::vector<onnxruntime::NodeArg*> inputs;
  std::vector<onnxruntime::NodeArg*> outputs;

  // FLOAT tensor.
  ONNX_NAMESPACE::TypeProto float_tensor;
  float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  // BOOL tensor.
  ONNX_NAMESPACE::TypeProto bool_tensor;
  bool_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_BOOL);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  bool_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  // UINT8 tensor.
  ONNX_NAMESPACE::TypeProto uint8_tensor;
  uint8_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_UINT8);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
  uint8_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

  auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &bool_tensor);
  auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &bool_tensor);
  inputs.push_back(&input_arg_1);
  inputs.push_back(&input_arg_2);
  auto& output_arg_1 = graph.GetOrCreateNodeArg("xor1_out", &bool_tensor);
  outputs.push_back(&output_arg_1);
  graph.AddNode("xor1", "Xor", "node 1.", inputs, outputs);

  inputs.clear();
  inputs.push_back(&output_arg_1);
  auto& output_arg_2 = graph.GetOrCreateNodeArg("not_out", &bool_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_2);
  graph.AddNode("not", "Not", "node 2.", inputs, outputs);

  auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &bool_tensor);
  inputs.clear();
  inputs.push_back(&output_arg_2);
  inputs.push_back(&input_arg_3);
  auto& output_arg_3 = graph.GetOrCreateNodeArg("xor2_out", &bool_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_3);
  graph.AddNode("xor2", "Xor", "node 3.", inputs, outputs);

  inputs.clear();
  inputs.push_back(&output_arg_2);
  inputs.push_back(&output_arg_3);
  auto& output_arg_4 = graph.GetOrCreateNodeArg("M", &bool_tensor);
  outputs.clear();
  outputs.push_back(&output_arg_4);
  graph.AddNode("and", "And", "node 4.", inputs, outputs);

  auto status = graph.Resolve();
  ASSERT_TRUE(status.IsOK());
  std::string model_file_name = "trt_execution_provider_removecycle_test.onnx";
  status = onnxruntime::Model::Save(model, model_file_name);

  std::vector<int64_t> dims_mul_x = {1, 3, 2};
  std::vector<bool> values_mul_x = {true, false, true, false, true, false};
  std::vector<int64_t> dims_mul_y = {1, 3, 2};
  std::vector<bool> values_mul_y = {true, true, false, true, false, false};
  std::vector<int64_t> dims_mul_z = {1, 3, 2};
  std::vector<bool> values_mul_z = {true, false, true, false, true, false};

  SessionOptions so;
  so.session_logid = "TensorrtExecutionProviderTest.RemoveCycleTest";
  RunOptions run_options;
  run_options.run_tag = so.session_logid;
  InferenceSession session_object{so, GetEnvironment()};

  auto allocator_manager = session_object.GetAllocatorManager();
  auto cuda_provider = DefaultCudaExecutionProvider();
  cuda_provider->RegisterAllocator(allocator_manager);
  auto cpu_allocator = cuda_provider->GetAllocator(0, OrtMemTypeCPU);

  OrtValue ml_value_x;
  CreateMLValue<bool>(cpu_allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<bool>(cpu_allocator, dims_mul_y, values_mul_y, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<bool>(cpu_allocator, dims_mul_y, values_mul_y, &ml_value_z);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("M");
  std::vector<OrtValue> fetches;

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_m = {1, 3, 2};
  std::vector<bool> expected_values_mul_m = {false, false, false, false, false, true};

  std::unique_ptr<IExecutionProvider> execution_provider = DefaultTensorrtExecutionProvider();
  EXPECT_TRUE(session_object.RegisterExecutionProvider(std::move(execution_provider)).IsOK());

  ASSERT_STATUS_OK(session_object.Load(model_file_name));
  ASSERT_STATUS_OK(session_object.Initialize());

  // Now run
  ASSERT_STATUS_OK(session_object.Run(run_options, feeds, output_names, &fetches));
  VerifyOutputs(fetches, expected_dims_mul_m, expected_values_mul_m);
}
}  // namespace test
}  // namespace onnxruntime
