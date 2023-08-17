// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/deoptimization-data.h"

#include <iomanip>

#include "src/deoptimizer/translated-state.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/objects/code.h"
#include "src/objects/deoptimization-data-inl.h"
#include "src/objects/shared-function-info.h"

#ifdef V8_USE_ZLIB
#include "third_party/zlib/google/compression_utils_portable.h"
#endif  // V8_USE_ZLIB

namespace v8 {
namespace internal {

Handle<DeoptimizationData> DeoptimizationData::New(Isolate* isolate,
                                                   int deopt_entry_count,
                                                   AllocationType allocation) {
  return Handle<DeoptimizationData>::cast(isolate->factory()->NewFixedArray(
      LengthFor(deopt_entry_count), allocation));
}

Handle<DeoptimizationData> DeoptimizationData::New(LocalIsolate* isolate,
                                                   int deopt_entry_count,
                                                   AllocationType allocation) {
  return Handle<DeoptimizationData>::cast(isolate->factory()->NewFixedArray(
      LengthFor(deopt_entry_count), allocation));
}

Handle<DeoptimizationData> DeoptimizationData::Empty(Isolate* isolate) {
  return Handle<DeoptimizationData>::cast(
      isolate->factory()->empty_fixed_array());
}

Handle<DeoptimizationData> DeoptimizationData::Empty(LocalIsolate* isolate) {
  return Handle<DeoptimizationData>::cast(
      isolate->factory()->empty_fixed_array());
}

SharedFunctionInfo DeoptimizationData::GetInlinedFunction(int index) {
  if (index == -1) {
    return SharedFunctionInfo::cast(SharedFunctionInfo());
  } else {
    return SharedFunctionInfo::cast(LiteralArray()->get(index));
  }
}

#ifdef ENABLE_DISASSEMBLER

namespace {
void print_pc(std::ostream& os, int pc) {
  if (pc == -1) {
    os << "NA";
  } else {
    os << std::hex << pc << std::dec;
  }
}
}  // namespace

void DeoptimizationData::PrintDeoptimizationData(std::ostream& os) const {
  if (length() == 0) {
    os << "Deoptimization Input Data invalidated by lazy deoptimization\n";
    return;
  }

  int const inlined_function_count = InlinedFunctionCount().value();
  os << "Inlined functions (count = " << inlined_function_count << ")\n";
  for (int id = 0; id < inlined_function_count; ++id) {
    Object info = LiteralArray()->get(id);
    os << " " << Brief(SharedFunctionInfo::cast(info)) << "\n";
  }
  os << "\n";
  int deopt_count = DeoptCount();
  os << "Deoptimization Input Data (deopt points = " << deopt_count << ")\n";
  if (0 != deopt_count) {
#ifdef DEBUG
    os << " index  bytecode-offset  node-id    pc";
#else   // DEBUG
    os << " index  bytecode-offset    pc";
#endif  // DEBUG
    if (v8_flags.print_code_verbose) os << "  commands";
    os << "\n";
  }
  for (int i = 0; i < deopt_count; i++) {
    os << std::setw(6) << i << "  " << std::setw(15)
       << GetBytecodeOffset(i).ToInt() << "  "
#ifdef DEBUG
       << std::setw(7) << NodeId(i).value() << "  "
#endif  // DEBUG
       << std::setw(4);
    print_pc(os, Pc(i).value());
    os << std::setw(2) << "\n";

    if (v8_flags.print_code_verbose) {
      FrameTranslation()->PrintFrameTranslation(os, TranslationIndex(i).value(),
                                                LiteralArray());
    }
  }
}

#endif  // ENABLE_DISASSEMBLER

DeoptimizationFrameTranslation::Iterator::Iterator(
    DeoptimizationFrameTranslation buffer, int index)
    : buffer_(buffer), index_(index) {
#ifdef V8_USE_ZLIB
  if (V8_UNLIKELY(v8_flags.turbo_compress_frame_translations)) {
    const int size = buffer_.get_int(kUncompressedSizeOffset);
    uncompressed_contents_.insert(uncompressed_contents_.begin(), size, 0);

    uLongf uncompressed_size =
        size * kDeoptimizationFrameTranslationElementSize;

    CHECK_EQ(zlib_internal::UncompressHelper(
                 zlib_internal::ZRAW,
                 base::bit_cast<Bytef*>(uncompressed_contents_.data()),
                 &uncompressed_size,
                 buffer_.GetDataStartAddress() + kCompressedDataOffset,
                 buffer_.DataSize()),
             Z_OK);
    DCHECK(index >= 0 && index < size);
    return;
  }
#endif  // V8_USE_ZLIB
  DCHECK(!v8_flags.turbo_compress_frame_translations);
  DCHECK(index >= 0 && index < buffer.length());
  // Starting at a location other than a BEGIN would make
  // MATCH_PREVIOUS_TRANSLATION instructions not work.
  DCHECK(TranslationOpcodeIsBegin(
      static_cast<TranslationOpcode>(buffer_.GetDataStartAddress()[index])));
}

int32_t DeoptimizationFrameTranslation::Iterator::NextOperand() {
  if (V8_UNLIKELY(v8_flags.turbo_compress_frame_translations)) {
    return uncompressed_contents_[index_++];
  } else if (remaining_ops_to_use_from_previous_translation_) {
    int32_t value =
        base::VLQDecode(buffer_.GetDataStartAddress(), &previous_index_);
    DCHECK_LT(previous_index_, index_);
    return value;
  } else {
    int32_t value = base::VLQDecode(buffer_.GetDataStartAddress(), &index_);
    DCHECK_LE(index_, buffer_.length());
    return value;
  }
}

TranslationOpcode
DeoptimizationFrameTranslation::Iterator::NextOpcodeAtPreviousIndex() {
  TranslationOpcode opcode =
      static_cast<TranslationOpcode>(buffer_.get(previous_index_++));
  DCHECK_LT(static_cast<uint32_t>(opcode), kNumTranslationOpcodes);
  DCHECK_NE(opcode, TranslationOpcode::MATCH_PREVIOUS_TRANSLATION);
  DCHECK_LT(previous_index_, index_);
  return opcode;
}

uint32_t
DeoptimizationFrameTranslation::Iterator::NextUnsignedOperandAtPreviousIndex() {
  uint32_t value =
      base::VLQDecodeUnsigned(buffer_.GetDataStartAddress(), &previous_index_);
  DCHECK_LT(previous_index_, index_);
  return value;
}

uint32_t DeoptimizationFrameTranslation::Iterator::NextOperandUnsigned() {
  if (V8_UNLIKELY(v8_flags.turbo_compress_frame_translations)) {
    return uncompressed_contents_[index_++];
  } else if (remaining_ops_to_use_from_previous_translation_) {
    return NextUnsignedOperandAtPreviousIndex();
  } else {
    uint32_t value =
        base::VLQDecodeUnsigned(buffer_.GetDataStartAddress(), &index_);
    DCHECK_LE(index_, buffer_.length());
    return value;
  }
}

TranslationOpcode DeoptimizationFrameTranslation::Iterator::NextOpcode() {
  if (V8_UNLIKELY(v8_flags.turbo_compress_frame_translations)) {
    return static_cast<TranslationOpcode>(NextOperandUnsigned());
  }
  if (remaining_ops_to_use_from_previous_translation_) {
    --remaining_ops_to_use_from_previous_translation_;
  }
  if (remaining_ops_to_use_from_previous_translation_) {
    return NextOpcodeAtPreviousIndex();
  }
  CHECK_LT(index_, buffer_.length());
  uint8_t opcode_byte = buffer_.get(index_++);

  // If the opcode byte is greater than any valid opcode, then the opcode is
  // implicitly MATCH_PREVIOUS_TRANSLATION and the operand is the opcode byte
  // minus kNumTranslationOpcodes. This special-case encoding of the most common
  // opcode saves some memory.
  if (opcode_byte >= kNumTranslationOpcodes) {
    remaining_ops_to_use_from_previous_translation_ =
        opcode_byte - kNumTranslationOpcodes;
    opcode_byte =
        static_cast<uint8_t>(TranslationOpcode::MATCH_PREVIOUS_TRANSLATION);
  } else if (opcode_byte ==
             static_cast<uint8_t>(
                 TranslationOpcode::MATCH_PREVIOUS_TRANSLATION)) {
    remaining_ops_to_use_from_previous_translation_ = NextOperandUnsigned();
  }

  TranslationOpcode opcode = static_cast<TranslationOpcode>(opcode_byte);
  DCHECK_LE(index_, buffer_.length());
  DCHECK_LT(static_cast<uint32_t>(opcode), kNumTranslationOpcodes);
  if (TranslationOpcodeIsBegin(opcode)) {
    int temp_index = index_;
    // The first argument for BEGIN is the distance, in bytes, since the
    // previous BEGIN, or zero to indicate that MATCH_PREVIOUS_TRANSLATION will
    // not be used in this translation.
    uint32_t lookback_distance =
        base::VLQDecodeUnsigned(buffer_.GetDataStartAddress(), &temp_index);
    if (lookback_distance) {
      previous_index_ = index_ - 1 - lookback_distance;
      DCHECK(TranslationOpcodeIsBegin(
          static_cast<TranslationOpcode>(buffer_.get(previous_index_))));
      // The previous BEGIN should specify zero as its lookback distance,
      // meaning it won't use MATCH_PREVIOUS_TRANSLATION.
      DCHECK_EQ(buffer_.get(previous_index_ + 1), 0);
    }
    ops_since_previous_index_was_updated_ = 1;
  } else if (opcode == TranslationOpcode::MATCH_PREVIOUS_TRANSLATION) {
    for (int i = 0; i < ops_since_previous_index_was_updated_; ++i) {
      SkipOpcodeAndItsOperandsAtPreviousIndex();
    }
    ops_since_previous_index_was_updated_ = 0;
    opcode = NextOpcodeAtPreviousIndex();
  } else {
    ++ops_since_previous_index_was_updated_;
  }
  return opcode;
}

bool DeoptimizationFrameTranslation::Iterator::HasNextOpcode() const {
  if (V8_UNLIKELY(v8_flags.turbo_compress_frame_translations)) {
    return index_ < static_cast<int>(uncompressed_contents_.size());
  } else {
    return index_ < buffer_.length() ||
           remaining_ops_to_use_from_previous_translation_ > 1;
  }
}

void DeoptimizationFrameTranslation::Iterator::
    SkipOpcodeAndItsOperandsAtPreviousIndex() {
  TranslationOpcode opcode = NextOpcodeAtPreviousIndex();
  for (int count = TranslationOpcodeOperandCount(opcode); count != 0; --count) {
    NextUnsignedOperandAtPreviousIndex();
  }
}

#ifdef ENABLE_DISASSEMBLER

void DeoptimizationFrameTranslation::PrintFrameTranslation(
    std::ostream& os, int index,
    DeoptimizationLiteralArray literal_array) const {
  DisallowGarbageCollection gc_oh_noes;

  DeoptimizationFrameTranslation::Iterator iterator(*this, index);
  TranslationOpcode opcode = iterator.NextOpcode();
  DCHECK(TranslationOpcodeIsBegin(opcode));
  os << opcode << " ";
  DeoptimizationFrameTranslationPrintSingleOpcode(os, opcode, iterator,
                                                  literal_array);
  while (iterator.HasNextOpcode()) {
    TranslationOpcode opcode = iterator.NextOpcode();
    if (TranslationOpcodeIsBegin(opcode)) {
      break;
    }
    os << opcode << " ";
    DeoptimizationFrameTranslationPrintSingleOpcode(os, opcode, iterator,
                                                    literal_array);
  }
}

#endif  // ENABLE_DISASSEMBLER

}  // namespace internal
}  // namespace v8
