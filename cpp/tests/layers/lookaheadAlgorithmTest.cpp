/*
 * Copyright (c) 2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <gtest/gtest.h>

#include "tensorrt_llm/layers/lookaheadAlgorithm.h"
#include "tensorrt_llm/layers/lookaheadDecodingUtils.h"
#include "tests/layers/randomLlm.h"

namespace tensorrt_llm::tests::layers
{
using namespace tensorrt_llm::runtime;
using namespace tensorrt_llm::layers;
using TensorPtr = runtime::ITensor::SharedPtr;

class LookaheadAlgorithmTest
    : public ::testing::TestWithParam<std::tuple<std::tuple<int, int>, std::tuple<int, int>, std::tuple<int, int>>>
{
};

bool verifyAcceptOffsets(TensorPtr output, TensorPtr accepted, TensorPtr acceptedOffsets)
{
    BufferRange<TokenIdType> outputRange(*output);
    BufferRange<TokenIdType> acceptedRange(*accepted);
    BufferRange<SizeType32> offsetsRange(*acceptedOffsets);
    bool result = true;
    for (SizeType32 i = 0; i < acceptedRange.size(); i++)
    {
        result &= outputRange[offsetsRange[i]] == acceptedRange[i];
    }
    return result;
}

TEST_P(LookaheadAlgorithmTest, predict)
{
    auto [Ww, Nn, Gg] = GetParam();
    auto [W, w] = Ww;
    auto [N, n] = Nn;
    auto [G, g] = Gg;

    auto ascii = std::make_shared<AsciiRandomTokenLogits>();

    std::string oracle(
        "The following example uses the following lambda-expression to increment all of the elements of a vector and "
        "then uses an overloaded operator() in a function object (a.k.a., \"functor\") to compute their sum. Note that "
        "to compute the sum, it is recommended to use the dedicated algorithm std::accumulate.&");
    LookaheadRandomLlm llm(ascii, oracle);

    auto prompt = initTensor(std::string(oracle.substr(0, 20)));
    BufferRange<TokenIdType> promptRange(*prompt);
    auto promptLen = ITensor::volume(prompt->getShape());

    auto maxSeqLen = 1024;
    auto maxDraftLen = (W + G) * (N - 1) - 1;
    auto shape = ITensor::makeShape({1 + maxDraftLen});
    auto shapeSingle = ITensor::makeShape({1});
    TensorPtr posidMax = BufferManager::cpu(shape, nvinfer1::DataType::kINT32);
    TensorPtr smaskMax = BufferManager::cpu(shape, nvinfer1::DataType::kBOOL);
    TensorPtr inputLengthPtr = BufferManager::cpu(shapeSingle, nvinfer1::DataType::kINT32);
    auto& inputLength(*BufferRange<SizeType32>(*inputLengthPtr).begin());

    TensorPtr outputMax = BufferManager::cpu(shape, nvinfer1::DataType::kINT32);
    TensorPtr endIdPtr = BufferManager::cpu(shapeSingle, nvinfer1::DataType::kINT32);
    auto& endId(*BufferRange<TokenIdType>(*endIdPtr).begin());
    endId = ascii->getEndToken();

    TensorPtr acceptedMax = BufferManager::cpu(shape, nvinfer1::DataType::kINT32);
    TensorPtr acceptedOffsetsMax = BufferManager::cpu(shape, nvinfer1::DataType::kINT32);
    TensorPtr acceptedLengthPtr = BufferManager::cpu(shapeSingle, nvinfer1::DataType::kINT32);
    auto& acceptedLength(*BufferRange<SizeType32>(*acceptedLengthPtr).begin());

    TensorPtr sequence = BufferManager::cpu(ITensor::makeShape({maxSeqLen + maxDraftLen}), nvinfer1::DataType::kINT32);
    BufferRange<TokenIdType> sequenceRange(*sequence);
    TensorPtr sequenceLengthPtr = BufferManager::cpu(shapeSingle, nvinfer1::DataType::kINT32);
    auto& sequenceLength(*bufferCast<SizeType32>(*sequenceLengthPtr));

    std::copy(promptRange.begin(), promptRange.end(), sequenceRange.begin());
    sequenceLength = promptLen;

    sequenceRange[sequenceLength] = oracle[promptLen]; // from context phase.
    sequenceLength += 1;

    PRINT_TOKENS(sequence);

    tensorrt_llm::layers::LookaheadAlgorithm algo(W, N, G);
    algo.setup(ITensor::slice(sequence, 0, sequenceLength), w, n, g);

    SizeType32 seqLen = oracle.size();
    std::vector<SizeType32> histogram(N + 1);

    for (; sequenceLength < seqLen;)
    {
        TLLM_LOG_DEBUG("\noracle[%d] = '%c'", sequenceLength - 1, static_cast<char>(sequenceRange[sequenceLength - 1]));
        bufferCast<SizeType32>(*posidMax)[0] = sequenceLength - 1;
        bufferCast<bool>(*smaskMax)[0] = true;
        algo.prepare(                                              //
            ITensor::slice(sequence, sequenceLength, maxDraftLen), //
            ITensor::slice(posidMax, 1, maxDraftLen),              //
            ITensor::slice(smaskMax, 1, maxDraftLen),              //
            inputLengthPtr,                                        //
            sequenceLengthPtr,                                     //
            ITensor::slice(sequence, sequenceLength - 1, 1));

        TensorPtr input = ITensor::slice(sequence, sequenceLength - 1, inputLength + 1);
        TensorPtr posid = ITensor::slice(posidMax, 0, inputLength + 1);
        TensorPtr smask = ITensor::slice(smaskMax, 0, inputLength + 1);

        PRINT_TOKENS(input);
        PRINT_VALUES(posid);
        PRINT_VALUES(smask);

        TensorPtr output = ITensor::slice(outputMax, 0, inputLength + 1);
        llm.foretell(output, input, posid);
        llm.sampleByMask(output, smask);
        PRINT_TOKENS(output);

        // algo.update(acceptedMax, acceptedOffsetsMax, acceptedLengthPtr, output, endIdPtr);
        algo.update(
            ITensor::slice(sequence, sequenceLength, N), acceptedOffsetsMax, acceptedLengthPtr, output, endIdPtr);

        TensorPtr accepted = ITensor::slice(sequence, sequenceLength, acceptedLength);
        TensorPtr acceptedOffsets = ITensor::slice(acceptedOffsetsMax, 0, acceptedLength);

        TLLM_CHECK(acceptedLength <= N);
        histogram[acceptedLength] += 1;
        PRINT_TOKENS(accepted);
        PRINT_VALUES(acceptedOffsets);

        EXPECT_TRUE(verifyAcceptOffsets(output, accepted, acceptedOffsets));
        EXPECT_TRUE(llm.verify(sequenceLength, accepted));

        sequenceLength += acceptedLength;

        TLLM_LOG_DEBUG("result: '%s'", D(ITensor::slice(sequence, 0, sequenceLength)).string().c_str());
    }
    EXPECT_EQ(sequenceLength, seqLen);

    TensorPtr hist = ITensor::wrap(histogram, ITensor::makeShape({N + 1}));
    TLLM_LOG_DEBUG("Lookahead acceptance histogram: %s", D(hist).values<SizeType32>().c_str());
}

INSTANTIATE_TEST_CASE_P(CombineLookaheadAlgorithmTest, LookaheadAlgorithmTest,
    testing::Combine( //
        testing::Values(std::make_tuple(1, 1), std::make_tuple(3, 3), std::make_tuple(5, 5), std::make_tuple(7, 7),
            std::make_tuple(3, 2), std::make_tuple(5, 3), std::make_tuple(7, 4)),
        testing::Values(std::make_tuple(3, 3), std::make_tuple(5, 5), std::make_tuple(7, 7), std::make_tuple(3, 2),
            std::make_tuple(5, 3), std::make_tuple(7, 4)),
        testing::Values(std::make_tuple(3, 3), std::make_tuple(5, 5), std::make_tuple(7, 7), std::make_tuple(3, 2),
            std::make_tuple(5, 3), std::make_tuple(7, 4))));

INSTANTIATE_TEST_CASE_P(CombineLookaheadAlgorithmTestSingleMax, LookaheadAlgorithmTest,
    testing::Combine(testing::Values(std::make_tuple(5, 5)), testing::Values(std::make_tuple(5, 5)),
        testing::Values(std::make_tuple(5, 5))));

INSTANTIATE_TEST_CASE_P(CombineLookaheadAlgorithmTestSingleDynamic, LookaheadAlgorithmTest,
    testing::Combine(testing::Values(std::make_tuple(3, 2)), testing::Values(std::make_tuple(3, 2)),
        testing::Values(std::make_tuple(3, 2))));

} // namespace tensorrt_llm::tests::layers
