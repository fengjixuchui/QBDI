
/*
 * This file is part of QBDI.
 *
 * Copyright 2017 - 2021 Quarkslab
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
#include <memory>
#include <stdint.h>
#include <vector>

#include "llvm/Support/Memory.h"

#include "QBDI/Config.h"
#include "QBDI/State.h"
#include "Engine/LLVMCPU.h"
#include "ExecBlock/ExecBlock.h"
#include "ExecBlock/X86_64/Context_X86_64.h"
#include "Patch/Patch.h"
#include "Patch/RelocatableInst.h"
#include "Patch/X86_64/PatchRules_X86_64.h"
#include "Utility/LogSys.h"

#if defined(QBDI_PLATFORM_WINDOWS)
extern "C" void qbdi_runCodeBlock(void *codeBlock, QBDI::rword execflags);
#else
extern void qbdi_runCodeBlock(void *codeBlock,
                              QBDI::rword execflags) asm("__qbdi_runCodeBlock");
#endif

namespace QBDI {

void ExecBlock::selectSeq(uint16_t seqID) {
  QBDI_REQUIRE(seqID < seqRegistry.size());
  currentSeq = seqID;
  currentInst = seqRegistry[currentSeq].startInstID;
  context->hostState.selector =
      reinterpret_cast<rword>(codeBlock.base()) +
      static_cast<rword>(instRegistry[currentInst].offset);
  context->hostState.executeFlags = seqRegistry[currentSeq].executeFlags;
}

void ExecBlock::run() {
  // Pages are RWX on iOS
  if constexpr (is_ios) {
    llvm::sys::Memory::InvalidateInstructionCache(codeBlock.base(),
                                                  codeBlock.allocatedSize());
  } else {
    if (not isRX()) {
      makeRX();
    }
  }
  qbdi_runCodeBlock(codeBlock.base(), context->hostState.executeFlags);
}

bool ExecBlock::writePatch(const Patch &p, const LLVMCPU &llvmcpu) {
  QBDI_REQUIRE(p.finalize);

  if (getEpilogueOffset() <= MINIMAL_BLOCK_SIZE) {
    isFull = true;
    return false;
  }

  for (const RelocatableInst::UniquePtr &inst : p.insts) {
    if (inst->getTag() != RelocatableInstTag::RelocInst) {
      QBDI_DEBUG("RelocTag 0x{:x}", inst->getTag());
      tagRegistry.push_back(
          TagInfo{static_cast<uint16_t>(inst->getTag()),
                  static_cast<uint16_t>(codeStream->current_pos())});
      continue;
    } else if (getEpilogueOffset() > MINIMAL_BLOCK_SIZE) {
      llvmcpu.writeInstruction(inst->reloc(this), codeStream.get());
    } else {
      QBDI_DEBUG("Not enough space left: rollback");
      return false;
    }
  }

  return true;
}

void ExecBlock::initScratchRegisterForPatch(
    std::vector<Patch>::const_iterator seqStart,
    std::vector<Patch>::const_iterator seqEnd) {}

void ExecBlock::finalizeScratchRegisterForPatch() {}

} // namespace QBDI
