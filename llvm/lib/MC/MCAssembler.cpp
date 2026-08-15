//===- lib/MC/MCAssembler.cpp - Assembler Backend Implementation ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCAssembler.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCCodeView.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSFrame.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

using namespace llvm;

namespace llvm {
class MCSubtargetInfo;
}

#define DEBUG_TYPE "assembler"

namespace {
constexpr uint64_t AtfieldPageSize = 4096;

struct AtfieldFunctionView {
  MCAtfieldFragment *Begin = nullptr;
  MCAtfieldFragment *End = nullptr;
  SmallVector<MCAtfieldFragment *, 8> Units;
};

static void writeFragment(raw_ostream &OS, const MCAssembler &Asm,
                          const MCFragment &F);

static bool atfieldMaterialize(const MCAssembler &Assembler,
                               const MCFragment &F,
                               SmallVectorImpl<char> &Contents) {
  switch (F.getKind()) {
  case MCFragment::FT_AtfieldAnchor:
  case MCFragment::FT_AtfieldMarker:
    return true;
  case MCFragment::FT_Data:
  case MCFragment::FT_Relaxable:
  case MCFragment::FT_Align:
  case MCFragment::FT_Fill:
  case MCFragment::FT_LEB:
  case MCFragment::FT_Nops:
  case MCFragment::FT_Org:
  case MCFragment::FT_Dwarf:
  case MCFragment::FT_DwarfFrame:
  case MCFragment::FT_SFrame:
  case MCFragment::FT_BoundaryAlign:
  case MCFragment::FT_SymbolId:
  case MCFragment::FT_CVInlineLines:
  case MCFragment::FT_CVDefRange:
    break;
  default:
    return false;
  }
  SmallString<64> Bytes;
  raw_svector_ostream Stream(Bytes);
  writeFragment(Stream, Assembler, F);
  Contents.append(Bytes.begin(), Bytes.end());
  return true;
}

static bool atfieldFixupCovers(const MCAssembler &Assembler,
                               const MCFragment &F, uint64_t Offset) {
  auto covers = [&](ArrayRef<MCFixup> Fixups) {
    for (const MCFixup &Fixup : Fixups) {
    uint64_t Width = 0;
    if (Fixup.getKind() >= FirstLiteralRelocationKind) {
      // A literal relocation kind is target-specific. Unknown x86-64 ELF
      // values are deliberately conservative and cover the entire fragment.
      if (Assembler.getContext().getTargetTriple().getArch() !=
              Triple::x86_64 ||
          !Assembler.getContext().getTargetTriple().isOSBinFormatELF())
        return true;
      switch (unsigned(Fixup.getKind()) - FirstLiteralRelocationKind) {
      case ELF::R_X86_64_NONE:
        continue;
      case ELF::R_X86_64_64:
      case ELF::R_X86_64_DTPMOD64:
      case ELF::R_X86_64_DTPOFF64:
      case ELF::R_X86_64_TPOFF64:
      case ELF::R_X86_64_PC64:
      case ELF::R_X86_64_GOTOFF64:
      case ELF::R_X86_64_GOT64:
      case ELF::R_X86_64_GOTPCREL64:
      case ELF::R_X86_64_GOTPC64:
      case ELF::R_X86_64_GOTPLT64:
      case ELF::R_X86_64_PLTOFF64:
      case ELF::R_X86_64_SIZE64:
        Width = 8;
        break;
      case ELF::R_X86_64_PC32:
      case ELF::R_X86_64_GOT32:
      case ELF::R_X86_64_PLT32:
      case ELF::R_X86_64_GOTPCREL:
      case ELF::R_X86_64_32:
      case ELF::R_X86_64_32S:
      case ELF::R_X86_64_TLSGD:
      case ELF::R_X86_64_TLSLD:
      case ELF::R_X86_64_DTPOFF32:
      case ELF::R_X86_64_GOTTPOFF:
      case ELF::R_X86_64_TPOFF32:
      case ELF::R_X86_64_GOTPC32:
      case ELF::R_X86_64_SIZE32:
      case ELF::R_X86_64_GOTPC32_TLSDESC:
      case ELF::R_X86_64_GOTPCRELX:
      case ELF::R_X86_64_REX_GOTPCRELX:
        Width = 4;
        break;
      case ELF::R_X86_64_16:
      case ELF::R_X86_64_PC16:
        Width = 2;
        break;
      case ELF::R_X86_64_8:
      case ELF::R_X86_64_PC8:
        Width = 1;
        break;
      default:
        return true;
      }
    } else {
      Width = (Assembler.getBackend().getFixupKindInfo(Fixup.getKind())
                   .TargetSize +
               7) /
              8;
    }
      if (Width == 0)
        Width = 1;
      if (Fixup.getOffset() <= Offset &&
          Offset - Fixup.getOffset() < Width)
        return true;
    }
    return false;
  };
  return covers(F.getFixups()) || covers(F.getVarFixups());
}

static bool atfieldFixupRewritesInstruction(const MCAssembler &,
                                            const MCFragment &F) {
  // Fixups are conservatively treated as instruction-rewriting.  This
  // deliberately excludes the complete fragment from natural-C3 candidacy,
  // including all target-specific relocation kinds.
  return !F.getFixups().empty() || !F.getVarFixups().empty();
}

namespace stats {

STATISTIC(EmittedFragments, "Number of emitted assembler fragments - total");
STATISTIC(EmittedRelaxableFragments,
          "Number of emitted assembler fragments - relaxable");
STATISTIC(EmittedDataFragments,
          "Number of emitted assembler fragments - data");
STATISTIC(EmittedAlignFragments,
          "Number of emitted assembler fragments - align");
STATISTIC(EmittedFillFragments,
          "Number of emitted assembler fragments - fill");
STATISTIC(EmittedNopsFragments, "Number of emitted assembler fragments - nops");
STATISTIC(EmittedOrgFragments, "Number of emitted assembler fragments - org");
STATISTIC(Fixups, "Number of fixups");
STATISTIC(FixupEvalForRelax, "Number of fixup evaluations for relaxation");
STATISTIC(ObjectBytes, "Number of emitted object file bytes");
STATISTIC(RelaxationSteps, "Number of assembler layout and relaxation steps");
STATISTIC(RelaxedInstructions, "Number of relaxed instructions");

} // end namespace stats
} // end anonymous namespace

// FIXME FIXME FIXME: There are number of places in this file where we convert
// what is a 64-bit assembler value used for computation into a value in the
// object file, which may truncate it. We should detect that truncation where
// invalid and report errors back.

/* *** */

MCAssembler::MCAssembler(MCContext &Context,
                         std::unique_ptr<MCAsmBackend> Backend,
                         std::unique_ptr<MCCodeEmitter> Emitter,
                         std::unique_ptr<MCObjectWriter> Writer)
    : Context(Context), Backend(std::move(Backend)),
      Emitter(std::move(Emitter)), Writer(std::move(Writer)) {
  if (this->Backend)
    this->Backend->setAssembler(this);
  if (this->Writer)
    this->Writer->setAssembler(this);
}

void MCAssembler::reset() {
  HasLayout = false;
  HasFinalLayout = false;
  RelaxAll = false;
  Sections.clear();
  AtfieldManifestSection = nullptr;
  AtfieldManifestStorage.clear();
  AtfieldManifestRecords.clear();
  Symbols.clear();
  ThumbFuncs.clear();

  // reset objects owned by us
  if (getBackendPtr())
    getBackendPtr()->reset();
  if (getEmitterPtr())
    getEmitterPtr()->reset();
  if (Writer)
    Writer->reset();
}

bool MCAssembler::registerSection(MCSection &Section) {
  if (Section.isRegistered())
    return false;
  Sections.push_back(&Section);
  Section.setIsRegistered(true);
  return true;
}

bool MCAssembler::isThumbFunc(const MCSymbol *Symbol) const {
  if (ThumbFuncs.count(Symbol))
    return true;

  if (!Symbol->isVariable())
    return false;

  const MCExpr *Expr = Symbol->getVariableValue();

  MCValue V;
  if (!Expr->evaluateAsRelocatable(V, nullptr))
    return false;

  if (V.getSubSym() || V.getSpecifier())
    return false;

  auto *Sym = V.getAddSym();
  if (!Sym || V.getSpecifier())
    return false;

  if (!isThumbFunc(Sym))
    return false;

  ThumbFuncs.insert(Symbol); // Cache it.
  return true;
}

bool MCAssembler::evaluateFixup(const MCFragment &F, MCFixup &Fixup,
                                MCValue &Target, uint64_t &Value,
                                bool RecordReloc, uint8_t *Data) const {
  if (RecordReloc)
    ++stats::Fixups;

  // FIXME: This code has some duplication with recordRelocation. We should
  // probably merge the two into a single callback that tries to evaluate a
  // fixup and records a relocation if one is needed.

  // On error claim to have completely evaluated the fixup, to prevent any
  // further processing from being done.
  const MCExpr *Expr = Fixup.getValue();
  Value = 0;
  if (!Expr->evaluateAsRelocatable(Target, this)) {
    reportError(Fixup.getLoc(), "expected relocatable expression");
    return true;
  }

  bool IsResolved = false;
  if (auto State = getBackend().evaluateFixup(F, Fixup, Target, Value)) {
    IsResolved = *State;
  } else {
    const MCSymbol *Add = Target.getAddSym();
    const MCSymbol *Sub = Target.getSubSym();
    Value += Target.getConstant();
    if (Add && Add->isDefined())
      Value += getSymbolOffset(*Add);
    if (Sub && Sub->isDefined())
      Value -= getSymbolOffset(*Sub);

    if (Fixup.isPCRel()) {
      Value -= getFragmentOffset(F) + Fixup.getOffset();
      if (Add && !Sub && !Add->isUndefined() && !Add->isAbsolute()) {
        IsResolved = getWriter().isSymbolRefDifferenceFullyResolvedImpl(
            *Add, F, false, true);
      }
    } else {
      IsResolved = Target.isAbsolute();
    }
  }

  if (!RecordReloc)
    return IsResolved;

  if (IsResolved && mc::isRelocRelocation(Fixup.getKind()))
    IsResolved = false;
  getBackend().applyFixup(F, Fixup, Target, Data, Value, IsResolved);
  return true;
}

uint64_t MCAssembler::computeFragmentSize(const MCFragment &F) const {
  assert(getBackendPtr() && "Requires assembler backend");
  switch (F.getKind()) {
  case MCFragment::FT_Data:
  case MCFragment::FT_Relaxable:
  case MCFragment::FT_Align:
  case MCFragment::FT_LEB:
  case MCFragment::FT_Dwarf:
  case MCFragment::FT_DwarfFrame:
  case MCFragment::FT_SFrame:
  case MCFragment::FT_CVInlineLines:
  case MCFragment::FT_CVDefRange:
    return F.getSize();
  case MCFragment::FT_Fill: {
    auto &FF = static_cast<const MCFillFragment &>(F);
    int64_t NumValues = 0;
    if (!FF.getNumValues().evaluateKnownAbsolute(NumValues, *this)) {
      recordError(FF.getLoc(), "expected assembly-time absolute expression");
      return 0;
    }
    int64_t Size = NumValues * FF.getValueSize();
    if (Size < 0) {
      recordError(FF.getLoc(), "invalid number of bytes");
      return 0;
    }
    return Size;
  }

  case MCFragment::FT_Nops:
    return cast<MCNopsFragment>(F).getNumBytes();

  case MCFragment::FT_BoundaryAlign:
    return cast<MCBoundaryAlignFragment>(F).getSize();

  case MCFragment::FT_SymbolId:
    return 4;
  case MCFragment::FT_AtfieldAnchor:
    return cast<MCAtfieldFragment>(F).isEnabled() ? 7 : 0;
  case MCFragment::FT_AtfieldMarker:
    return 0;

  case MCFragment::FT_Org: {
    const MCOrgFragment &OF = cast<MCOrgFragment>(F);
    MCValue Value;
    if (!OF.getOffset().evaluateAsValue(Value, *this)) {
      recordError(OF.getLoc(), "expected assembly-time absolute expression");
      return 0;
    }

    uint64_t FragmentOffset = getFragmentOffset(OF);
    int64_t TargetLocation = Value.getConstant();
    if (const auto *SA = Value.getAddSym()) {
      uint64_t Val;
      if (!getSymbolOffset(*SA, Val)) {
        recordError(OF.getLoc(), "expected absolute expression");
        return 0;
      }
      TargetLocation += Val;
    }
    int64_t Size = TargetLocation - FragmentOffset;
    if (Size < 0 || Size >= 0x40000000) {
      recordError(OF.getLoc(), "invalid .org offset '" + Twine(TargetLocation) +
                                   "' (at offset '" + Twine(FragmentOffset) +
                                   "')");
      return 0;
    }
    return Size;
  }

  }

  llvm_unreachable("invalid fragment kind");
}

// Simple getSymbolOffset helper for the non-variable case.
static bool getLabelOffset(const MCAssembler &Asm, const MCSymbol &S,
                           bool ReportError, uint64_t &Val) {
  if (!S.getFragment()) {
    if (ReportError)
      reportFatalUsageError("cannot evaluate undefined symbol '" + S.getName() +
                            "'");
    return false;
  }
  Val = Asm.getFragmentOffset(*S.getFragment()) + S.getOffset();
  return true;
}

static bool getSymbolOffsetImpl(const MCAssembler &Asm, const MCSymbol &S,
                                bool ReportError, uint64_t &Val) {
  if (!S.isVariable())
    return getLabelOffset(Asm, S, ReportError, Val);

  // If SD is a variable, evaluate it.
  MCValue Target;
  if (!S.getVariableValue()->evaluateAsValue(Target, Asm))
    reportFatalUsageError("cannot evaluate equated symbol '" + S.getName() +
                          "'");

  uint64_t Offset = Target.getConstant();

  const MCSymbol *A = Target.getAddSym();
  if (A) {
    uint64_t ValA;
    // FIXME: On most platforms, `Target`'s component symbols are labels from
    // having been simplified during evaluation, but on Mach-O they can be
    // variables due to PR19203. This, and the line below for `B` can be
    // restored to call `getLabelOffset` when PR19203 is fixed.
    if (!getSymbolOffsetImpl(Asm, *A, ReportError, ValA))
      return false;
    Offset += ValA;
  }

  const MCSymbol *B = Target.getSubSym();
  if (B) {
    uint64_t ValB;
    if (!getSymbolOffsetImpl(Asm, *B, ReportError, ValB))
      return false;
    Offset -= ValB;
  }

  Val = Offset;
  return true;
}

bool MCAssembler::getSymbolOffset(const MCSymbol &S, uint64_t &Val) const {
  return getSymbolOffsetImpl(*this, S, false, Val);
}

uint64_t MCAssembler::getSymbolOffset(const MCSymbol &S) const {
  uint64_t Val;
  getSymbolOffsetImpl(*this, S, true, Val);
  return Val;
}

const MCSymbol *MCAssembler::getBaseSymbol(const MCSymbol &Symbol) const {
  assert(HasLayout);
  if (!Symbol.isVariable())
    return &Symbol;

  const MCExpr *Expr = Symbol.getVariableValue();
  MCValue Value;
  if (!Expr->evaluateAsValue(Value, *this)) {
    reportError(Expr->getLoc(), "expression could not be evaluated");
    return nullptr;
  }

  const MCSymbol *SymB = Value.getSubSym();
  if (SymB) {
    reportError(Expr->getLoc(),
                Twine("symbol '") + SymB->getName() +
                    "' could not be evaluated in a subtraction expression");
    return nullptr;
  }

  const MCSymbol *A = Value.getAddSym();
  if (!A)
    return nullptr;

  const MCSymbol &ASym = *A;
  if (ASym.isCommon()) {
    reportError(Expr->getLoc(), "Common symbol '" + ASym.getName() +
                                    "' cannot be used in assignment expr");
    return nullptr;
  }

  return &ASym;
}

uint64_t MCAssembler::getSectionAddressSize(const MCSection &Sec) const {
  const MCFragment &F = *Sec.curFragList()->Tail;
  assert(HasLayout && F.getKind() == MCFragment::FT_Data);
  return getFragmentOffset(F) + F.getSize();
}

uint64_t MCAssembler::getSectionFileSize(const MCSection &Sec) const {
  // Virtual sections have no file size.
  if (Sec.isBssSection())
    return 0;
  return getSectionAddressSize(Sec);
}

bool MCAssembler::registerSymbol(const MCSymbol &Symbol) {
  bool Changed = !Symbol.isRegistered();
  if (Changed) {
    Symbol.setIsRegistered(true);
    Symbols.push_back(&Symbol);
  }
  return Changed;
}

void MCAssembler::addRelocDirective(RelocDirective RD) {
  relocDirectives.push_back(RD);
}

namespace {
/// Write the fragment \p F to the output file.
static void writeFragment(raw_ostream &OS, const MCAssembler &Asm,
                          const MCFragment &F) {
  // FIXME: Embed in fragments instead?
  uint64_t FragmentSize = Asm.computeFragmentSize(F);

  llvm::endianness Endian = Asm.getBackend().Endian;

  // This variable (and its dummy usage) is to participate in the assert at
  // the end of the function.
  uint64_t Start = OS.tell();
  (void) Start;

  ++stats::EmittedFragments;

  switch (F.getKind()) {
  case MCFragment::FT_Data:
  case MCFragment::FT_Relaxable:
  case MCFragment::FT_LEB:
  case MCFragment::FT_Dwarf:
  case MCFragment::FT_DwarfFrame:
  case MCFragment::FT_SFrame:
  case MCFragment::FT_CVInlineLines:
  case MCFragment::FT_CVDefRange: {
    if (F.getKind() == MCFragment::FT_Data)
      ++stats::EmittedDataFragments;
    else if (F.getKind() == MCFragment::FT_Relaxable)
      ++stats::EmittedRelaxableFragments;
    const auto &EF = cast<MCFragment>(F);
    OS << StringRef(EF.getContents().data(), EF.getContents().size());
    OS << StringRef(EF.getVarContents().data(), EF.getVarContents().size());
  } break;

  case MCFragment::FT_Align: {
    ++stats::EmittedAlignFragments;
    OS << StringRef(F.getContents().data(), F.getContents().size());
    assert(F.getAlignFillLen() &&
           "Invalid virtual align in concrete fragment!");

    uint64_t Count = (FragmentSize - F.getFixedSize()) / F.getAlignFillLen();
    assert((FragmentSize - F.getFixedSize()) % F.getAlignFillLen() == 0 &&
           "computeFragmentSize computed size is incorrect");

    // In the nops mode, call the backend hook to write `Count` nops.
    if (F.hasAlignEmitNops()) {
      if (!Asm.getBackend().writeNopData(OS, Count, F.getSubtargetInfo()))
        reportFatalInternalError("unable to write nop sequence of " +
                                 Twine(Count) + " bytes");
    } else {
      // Otherwise, write out in multiples of the value size.
      for (uint64_t i = 0; i != Count; ++i) {
        switch (F.getAlignFillLen()) {
        default:
          llvm_unreachable("Invalid size!");
        case 1:
          OS << char(F.getAlignFill());
          break;
        case 2:
          support::endian::write<uint16_t>(OS, F.getAlignFill(), Endian);
          break;
        case 4:
          support::endian::write<uint32_t>(OS, F.getAlignFill(), Endian);
          break;
        case 8:
          support::endian::write<uint64_t>(OS, F.getAlignFill(), Endian);
          break;
        }
      }
    }
  } break;

  case MCFragment::FT_Fill: {
    ++stats::EmittedFillFragments;
    const MCFillFragment &FF = cast<MCFillFragment>(F);
    uint64_t V = FF.getValue();
    unsigned VSize = FF.getValueSize();
    const unsigned MaxChunkSize = 16;
    char Data[MaxChunkSize];
    assert(0 < VSize && VSize <= MaxChunkSize && "Illegal fragment fill size");
    // Duplicate V into Data as byte vector to reduce number of
    // writes done. As such, do endian conversion here.
    for (unsigned I = 0; I != VSize; ++I) {
      unsigned index = Endian == llvm::endianness::little ? I : (VSize - I - 1);
      Data[I] = uint8_t(V >> (index * 8));
    }
    for (unsigned I = VSize; I < MaxChunkSize; ++I)
      Data[I] = Data[I - VSize];

    // Set to largest multiple of VSize in Data.
    const unsigned NumPerChunk = MaxChunkSize / VSize;
    // Set ChunkSize to largest multiple of VSize in Data
    const unsigned ChunkSize = VSize * NumPerChunk;

    // Do copies by chunk.
    StringRef Ref(Data, ChunkSize);
    for (uint64_t I = 0, E = FragmentSize / ChunkSize; I != E; ++I)
      OS << Ref;

    // do remainder if needed.
    unsigned TrailingCount = FragmentSize % ChunkSize;
    if (TrailingCount)
      OS.write(Data, TrailingCount);
    break;
  }

  case MCFragment::FT_Nops: {
    ++stats::EmittedNopsFragments;
    const MCNopsFragment &NF = cast<MCNopsFragment>(F);

    int64_t NumBytes = NF.getNumBytes();
    int64_t ControlledNopLength = NF.getControlledNopLength();
    int64_t MaximumNopLength =
        Asm.getBackend().getMaximumNopSize(*NF.getSubtargetInfo());

    assert(NumBytes > 0 && "Expected positive NOPs fragment size");
    assert(ControlledNopLength >= 0 && "Expected non-negative NOP size");

    if (ControlledNopLength > MaximumNopLength) {
      Asm.reportError(NF.getLoc(), "illegal NOP size " +
                                       std::to_string(ControlledNopLength) +
                                       ". (expected within [0, " +
                                       std::to_string(MaximumNopLength) + "])");
      // Clamp the NOP length as reportError does not stop the execution
      // immediately.
      ControlledNopLength = MaximumNopLength;
    }

    // Use maximum value if the size of each NOP is not specified
    if (!ControlledNopLength)
      ControlledNopLength = MaximumNopLength;

    while (NumBytes) {
      uint64_t NumBytesToEmit =
          (uint64_t)std::min(NumBytes, ControlledNopLength);
      assert(NumBytesToEmit && "try to emit empty NOP instruction");
      if (!Asm.getBackend().writeNopData(OS, NumBytesToEmit,
                                         NF.getSubtargetInfo())) {
        report_fatal_error("unable to write nop sequence of the remaining " +
                           Twine(NumBytesToEmit) + " bytes");
        break;
      }
      NumBytes -= NumBytesToEmit;
    }
    break;
  }

  case MCFragment::FT_BoundaryAlign: {
    const MCBoundaryAlignFragment &BF = cast<MCBoundaryAlignFragment>(F);
    if (!Asm.getBackend().writeNopData(OS, FragmentSize, BF.getSubtargetInfo()))
      report_fatal_error("unable to write nop sequence of " +
                         Twine(FragmentSize) + " bytes");
    break;
  }

  case MCFragment::FT_SymbolId: {
    const MCSymbolIdFragment &SF = cast<MCSymbolIdFragment>(F);
    support::endian::write<uint32_t>(OS, SF.getSymbol()->getIndex(), Endian);
    break;
  }

  case MCFragment::FT_Org: {
    ++stats::EmittedOrgFragments;
    const MCOrgFragment &OF = cast<MCOrgFragment>(F);

    for (uint64_t i = 0, e = FragmentSize; i != e; ++i)
      OS << char(OF.getValue());

    break;
  }

  case MCFragment::FT_AtfieldAnchor:
  case MCFragment::FT_AtfieldMarker: {
    const auto &AF = cast<MCAtfieldFragment>(F);
    if (AF.isEnabled()) {
      static constexpr char Wrapper[] = {
          char(0x0f), char(0x1f), char(0x80), char(0xc3),
          char(0x00), char(0x00), char(0x00)};
      OS.write(Wrapper, sizeof(Wrapper));
      assert(FragmentSize == sizeof(Wrapper) &&
             "invalid ATField anchor size");
    } else {
      assert(FragmentSize == 0 && "ATField marker must be zero-sized");
    }
    break;
  }
  }

  assert(OS.tell() - Start == FragmentSize &&
         "The stream should advance by fragment size");
}
} // namespace

void MCAssembler::writeSectionData(raw_ostream &OS,
                                   const MCSection *Sec) const {
  assert(getBackendPtr() && "Expected assembler backend");

  if (Sec->isBssSection()) {
    assert(getSectionFileSize(*Sec) == 0 && "Invalid size for section!");

    // Ensure no fixups or non-zero bytes are written to BSS sections, catching
    // errors in both input assembly code and MCStreamer API usage. Location is
    // not tracked for efficiency.
    auto Fn = [](char c) { return c != 0; };
    for (const MCFragment &F : *Sec) {
      bool HasNonZero = false;
      switch (F.getKind()) {
      default:
        reportFatalInternalError("BSS section '" + Sec->getName() +
                                 "' contains invalid fragment");
        break;
      case MCFragment::FT_Data:
      case MCFragment::FT_Relaxable:
        HasNonZero =
            any_of(F.getContents(), Fn) || any_of(F.getVarContents(), Fn);
        break;
      case MCFragment::FT_Align:
        // Disallowed for API usage. AsmParser changes non-zero fill values to
        // 0.
        assert(F.getAlignFill() == 0 && "Invalid align in virtual section!");
        break;
      case MCFragment::FT_Fill:
        HasNonZero = cast<MCFillFragment>(F).getValue() != 0;
        break;
      case MCFragment::FT_Org:
        HasNonZero = cast<MCOrgFragment>(F).getValue() != 0;
        break;
      }
      if (HasNonZero) {
        reportError(SMLoc(), "BSS section '" + Sec->getName() +
                                 "' cannot have non-zero bytes");
        break;
      }
      if (F.getFixups().size() || F.getVarFixups().size()) {
        reportError(SMLoc(),
                    "BSS section '" + Sec->getName() + "' cannot have fixups");
        break;
      }
    }

    return;
  }

  uint64_t Start = OS.tell();
  (void)Start;

  for (const MCFragment &F : *Sec)
    writeFragment(OS, *this, F);

  flushPendingErrors();
  assert(getContext().hadError() ||
         OS.tell() - Start == getSectionAddressSize(*Sec));
}

void MCAssembler::layout() {
  assert(getBackendPtr() && "Expected assembler backend");
  DEBUG_WITH_TYPE("mc-dump-pre", {
    errs() << "assembler backend - pre-layout\n--\n";
    dump();
  });

  bool HasAtfield = false;
  for (MCSection &Sec : *this)
    for (MCFragment &Frag : Sec)
      HasAtfield |= isa<MCAtfieldFragment>(&Frag);
  if (HasAtfield && !AtfieldManifestSection &&
      Context.getTargetTriple().isOSBinFormatELF()) {
    AtfieldManifestSection =
        Context.getELFSection(".note.atfield.anchors", ELF::SHT_NOTE, 0);
    AtfieldManifestSection->setAlignment(Align(4));
    if (!AtfieldManifestSection->CurFragList) {
      AtfieldManifestSection->Subsections.push_back(
          {0u, MCSection::FragList{}});
      AtfieldManifestSection->CurFragList =
          &AtfieldManifestSection->Subsections.back().second;
    }
    registerSection(*AtfieldManifestSection);
  }

  // Assign section ordinals.
  unsigned SectionIndex = 0;
  for (MCSection &Sec : *this) {
    Sec.setOrdinal(SectionIndex++);

    // Chain together fragments from all subsections.
    if (Sec.Subsections.size() > 1) {
      MCFragment Dummy;
      MCFragment *Tail = &Dummy;
      for (auto &[_, List] : Sec.Subsections) {
        assert(List.Head);
        Tail->Next = List.Head;
        Tail = List.Tail;
      }
      Sec.Subsections.clear();
      Sec.Subsections.push_back({0u, {Dummy.getNext(), Tail}});
      Sec.CurFragList = &Sec.Subsections[0].second;

      unsigned FragmentIndex = 0;
      for (MCFragment &Frag : Sec)
        Frag.setLayoutOrder(FragmentIndex++);
    }
  }

  // Layout until everything fits.
  this->HasLayout = true;
  for (MCSection &Sec : *this)
    layoutSection(Sec);
  while (true) {
    unsigned FirstStable = Sections.size();
    while ((FirstStable = relaxOnce(FirstStable)) > 0)
      if (getContext().hadError())
        return;
    if (getContext().hadError())
      return;
    const bool AnchorEnabled = prepareAtfieldAnchors();
    if (getContext().hadError())
      return;
    if (!AnchorEnabled)
      break;
    for (MCSection &Sec : *this)
      layoutSection(Sec);
    // The newly enabled fragments change offsets and may trigger branch
    // relaxation; repeat until the monotonic anchor set is stable.
  }
  // Some targets might adjust fragment offsets in finishLayout().  Anchors
  // can themselves change those offsets, so keep the target finalization and
  // monotonic anchor preparation in one fixed point.
  while (true) {
    if (getBackend().finishLayout()) {
      for (MCSection &Sec : *this)
        layoutSection(Sec);
    }
    unsigned FirstStable = Sections.size();
    while ((FirstStable = relaxOnce(FirstStable)) > 0)
      if (getContext().hadError())
        return;
    if (getContext().hadError())
      return;
    const bool AnchorEnabled = prepareAtfieldAnchors();
    if (getContext().hadError())
      return;
    if (!AnchorEnabled)
      break;
    for (MCSection &Sec : *this)
      layoutSection(Sec);
  }
  emitAtfieldSymbols();
  emitAtfieldManifest();

  flushPendingErrors();

  DEBUG_WITH_TYPE("mc-dump", {
      errs() << "assembler backend - final-layout\n--\n";
      dump(); });

  // Allow the object writer a chance to perform post-layout binding (for
  // example, to set the index fields in the symbol data).
  getWriter().executePostLayoutBinding();

  // Fragment sizes are finalized. For RISC-V linker relaxation, this flag
  // helps check whether a PC-relative fixup is fully resolved.
  this->HasFinalLayout = true;

  // Resolve .reloc offsets and add fixups.
  for (auto &PF : relocDirectives) {
    MCValue Res;
    auto &O = PF.Offset;
    if (!O.evaluateAsValue(Res, *this)) {
      getContext().reportError(O.getLoc(), ".reloc offset is not relocatable");
      continue;
    }
    auto *Sym = Res.getAddSym();
    auto *F = Sym ? Sym->getFragment() : nullptr;
    auto *Sec = F ? F->getParent() : nullptr;
    if (Res.getSubSym() || !Sec) {
      getContext().reportError(O.getLoc(),
                               ".reloc offset is not relative to a section");
      continue;
    }

    uint64_t Offset = Sym ? Sym->getOffset() + Res.getConstant() : 0;
    F->addFixup(MCFixup::create(Offset, PF.Expr, PF.Kind));
  }

  // Evaluate and apply the fixups, generating relocation entries as necessary.
  for (MCSection &Sec : *this) {
    for (MCFragment &F : Sec) {
      // Process fragments with fixups here.
      auto Contents = F.getContents();
      for (MCFixup &Fixup : F.getFixups()) {
        uint64_t FixedValue;
        MCValue Target;
        assert(mc::isRelocRelocation(Fixup.getKind()) ||
               Fixup.getOffset() <= F.getFixedSize());
        auto *Data =
            reinterpret_cast<uint8_t *>(Contents.data() + Fixup.getOffset());
        evaluateFixup(F, Fixup, Target, FixedValue,
                      /*RecordReloc=*/true, Data);
      }
      // In the variable part, fixup offsets are relative to the fixed part's
      // start.
      for (MCFixup &Fixup : F.getVarFixups()) {
        uint64_t FixedValue;
        MCValue Target;
        assert(mc::isRelocRelocation(Fixup.getKind()) ||
               (Fixup.getOffset() >= F.getFixedSize() &&
                Fixup.getOffset() <= F.getSize()));
        auto *Data = reinterpret_cast<uint8_t *>(
            F.getVarContents().data() + (Fixup.getOffset() - F.getFixedSize()));
        evaluateFixup(F, Fixup, Target, FixedValue,
                      /*RecordReloc=*/true, Data);
      }
    }
  }
}

void MCAssembler::Finish() {
  layout();

  // Write the object file.
  stats::ObjectBytes += getWriter().writeObject();

  HasLayout = false;
  assert(PendingErrors.empty());
}

bool MCAssembler::fixupNeedsRelaxation(const MCFragment &F,
                                       const MCFixup &Fixup) const {
  ++stats::FixupEvalForRelax;
  MCValue Target;
  uint64_t Value;
  bool Resolved = evaluateFixup(F, const_cast<MCFixup &>(Fixup), Target, Value,
                                /*RecordReloc=*/false, {});
  return getBackend().fixupNeedsRelaxationAdvanced(F, Fixup, Target, Value,
                                                   Resolved);
}

void MCAssembler::relaxInstruction(MCFragment &F) {
  assert(getEmitterPtr() &&
         "Expected CodeEmitter defined for relaxInstruction");
  // If this inst doesn't ever need relaxation, ignore it. This occurs when we
  // are intentionally pushing out inst fragments, or because we relaxed a
  // previous instruction to one that doesn't need relaxation.
  if (!getBackend().mayNeedRelaxation(F.getOpcode(), F.getOperands(),
                                      *F.getSubtargetInfo()))
    return;

  bool DoRelax = false;
  for (const MCFixup &Fixup : F.getVarFixups())
    if ((DoRelax = fixupNeedsRelaxation(F, Fixup)))
      break;
  if (!DoRelax)
    return;

  ++stats::RelaxedInstructions;

  // TODO Refactor relaxInstruction to accept MCFragment and remove
  // `setInst`.
  MCInst Relaxed = F.getInst();
  getBackend().relaxInstruction(Relaxed, *F.getSubtargetInfo());

  // Encode the new instruction.
  F.setInst(Relaxed);
  SmallVector<char, 16> Data;
  SmallVector<MCFixup, 1> Fixups;
  getEmitter().encodeInstruction(Relaxed, Data, Fixups, *F.getSubtargetInfo());
  F.setVarContents(Data);
  F.setVarFixups(Fixups);
}

void MCAssembler::relaxLEB(MCFragment &F) {
  unsigned PadTo = F.getVarSize();
  int64_t Value;
  F.clearVarFixups();
  // Use evaluateKnownAbsolute for Mach-O as a hack: .subsections_via_symbols
  // requires that .uleb128 A-B is foldable where A and B reside in different
  // fragments. This is used by __gcc_except_table.
  bool Abs = getWriter().getSubsectionsViaSymbols()
                 ? F.getLEBValue().evaluateKnownAbsolute(Value, *this)
                 : F.getLEBValue().evaluateAsAbsolute(Value, *this);
  if (!Abs) {
    bool Relaxed, UseZeroPad;
    std::tie(Relaxed, UseZeroPad) = getBackend().relaxLEB128(F, Value);
    if (!Relaxed) {
      reportError(F.getLEBValue().getLoc(),
                  Twine(F.isLEBSigned() ? ".s" : ".u") +
                      "leb128 expression is not absolute");
      F.setLEBValue(MCConstantExpr::create(0, Context));
    }
    uint8_t Tmp[10]; // maximum size: ceil(64/7)
    PadTo = std::max(PadTo, encodeULEB128(uint64_t(Value), Tmp));
    if (UseZeroPad)
      Value = 0;
  }
  uint8_t Data[16];
  size_t Size = 0;
  // The compiler can generate EH table assembly that is impossible to assemble
  // without either adding padding to an LEB fragment or adding extra padding
  // to a later alignment fragment. To accommodate such tables, relaxation can
  // only increase an LEB fragment size here, not decrease it. See PR35809.
  if (F.isLEBSigned())
    Size = encodeSLEB128(Value, Data, PadTo);
  else
    Size = encodeULEB128(Value, Data, PadTo);
  F.setVarContents({reinterpret_cast<char *>(Data), Size});
}

/// Check if the branch crosses the boundary.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch cross the boundary.
static bool mayCrossBoundary(uint64_t StartAddr, uint64_t Size,
                             Align BoundaryAlignment) {
  uint64_t EndAddr = StartAddr + Size;
  return (StartAddr >> Log2(BoundaryAlignment)) !=
         ((EndAddr - 1) >> Log2(BoundaryAlignment));
}

/// Check if the branch is against the boundary.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch is against the boundary.
static bool isAgainstBoundary(uint64_t StartAddr, uint64_t Size,
                              Align BoundaryAlignment) {
  uint64_t EndAddr = StartAddr + Size;
  return (EndAddr & (BoundaryAlignment.value() - 1)) == 0;
}

/// Check if the branch needs padding.
///
/// \param StartAddr start address of the fused/unfused branch.
/// \param Size size of the fused/unfused branch.
/// \param BoundaryAlignment alignment requirement of the branch.
/// \returns true if the branch needs padding.
static bool needPadding(uint64_t StartAddr, uint64_t Size,
                        Align BoundaryAlignment) {
  return mayCrossBoundary(StartAddr, Size, BoundaryAlignment) ||
         isAgainstBoundary(StartAddr, Size, BoundaryAlignment);
}

void MCAssembler::relaxBoundaryAlign(MCBoundaryAlignFragment &BF) {
  // BoundaryAlignFragment that doesn't need to align any fragment should not be
  // relaxed.
  if (!BF.getLastFragment())
    return;

  uint64_t AlignedOffset = getFragmentOffset(BF);
  uint64_t AlignedSize = 0;
  for (const MCFragment *F = BF.getNext();; F = F->getNext()) {
    AlignedSize += computeFragmentSize(*F);
    if (F == BF.getLastFragment())
      break;
  }

  Align BoundaryAlignment = BF.getAlignment();
  uint64_t NewSize = needPadding(AlignedOffset, AlignedSize, BoundaryAlignment)
                         ? offsetToAlignment(AlignedOffset, BoundaryAlignment)
                         : 0U;
  if (NewSize == BF.getSize())
    return;
  BF.setSize(NewSize);
}

void MCAssembler::relaxDwarfLineAddr(MCFragment &F) {
  if (getBackend().relaxDwarfLineAddr(F))
    return;

  MCContext &Context = getContext();
  int64_t AddrDelta;
  bool Abs = F.getDwarfAddrDelta().evaluateKnownAbsolute(AddrDelta, *this);
  assert(Abs && "We created a line delta with an invalid expression");
  (void)Abs;
  SmallVector<char, 8> Data;
  MCDwarfLineAddr::encode(Context, getDWARFLinetableParams(),
                          F.getDwarfLineDelta(), AddrDelta, Data);
  F.setVarContents(Data);
  F.clearVarFixups();
}

void MCAssembler::relaxDwarfCallFrameFragment(MCFragment &F) {
  if (getBackend().relaxDwarfCFA(F))
    return;

  MCContext &Context = getContext();
  int64_t Value;
  bool Abs = F.getDwarfAddrDelta().evaluateAsAbsolute(Value, *this);
  if (!Abs) {
    reportError(F.getDwarfAddrDelta().getLoc(),
                "invalid CFI advance_loc expression");
    F.setDwarfAddrDelta(MCConstantExpr::create(0, Context));
    return;
  }

  SmallVector<char, 8> Data;
  MCDwarfFrameEmitter::encodeAdvanceLoc(Context, Value, Data);
  F.setVarContents(Data);
  F.clearVarFixups();
}

void MCAssembler::relaxSFrameFragment(MCFragment &F) {
  assert(F.getKind() == MCFragment::FT_SFrame);
  MCContext &C = getContext();
  int64_t Value;
  bool Abs = F.getSFrameAddrDelta().evaluateAsAbsolute(Value, *this);
  if (!Abs) {
    C.reportError(F.getSFrameAddrDelta().getLoc(),
                  "invalid CFI advance_loc expression in sframe");
    F.setSFrameAddrDelta(MCConstantExpr::create(0, C));
    return;
  }

  SmallVector<char, 4> Data;
  MCSFrameEmitter::encodeFuncOffset(Context, Value, Data, F.getSFrameFDE());
  F.setVarContents(Data);
  F.clearVarFixups();
}

bool MCAssembler::relaxFragment(MCFragment &F) {
  auto Size = computeFragmentSize(F);
  switch (F.getKind()) {
  default:
    return false;
  case MCFragment::FT_Relaxable:
    assert(!getRelaxAll() && "Did not expect a FT_Relaxable in RelaxAll mode");
    relaxInstruction(F);
    break;
  case MCFragment::FT_LEB:
    relaxLEB(F);
    break;
  case MCFragment::FT_Dwarf:
    relaxDwarfLineAddr(F);
    break;
  case MCFragment::FT_DwarfFrame:
    relaxDwarfCallFrameFragment(F);
    break;
  case MCFragment::FT_SFrame:
    relaxSFrameFragment(F);
    break;
  case MCFragment::FT_BoundaryAlign:
    relaxBoundaryAlign(static_cast<MCBoundaryAlignFragment &>(F));
    break;
  case MCFragment::FT_CVInlineLines:
    getContext().getCVContext().encodeInlineLineTable(
        *this, static_cast<MCCVInlineLineTableFragment &>(F));
    break;
  case MCFragment::FT_CVDefRange:
    getContext().getCVContext().encodeDefRange(
        *this, static_cast<MCCVDefRangeFragment &>(F));
    break;
  case MCFragment::FT_Fill:
  case MCFragment::FT_Org:
    return F.getNext()->Offset - F.Offset != Size;
  }
  return computeFragmentSize(F) != Size;
}

void MCAssembler::layoutSection(MCSection &Sec) {
  uint64_t Offset = 0;
  for (MCFragment &F : Sec) {
    F.Offset = Offset;
    if (F.getKind() == MCFragment::FT_Align) {
      Offset += F.getFixedSize();
      unsigned Size = offsetToAlignment(Offset, F.getAlignment());
      // In the nops mode, RISC-V style linker relaxation might adjust the size
      // and add a fixup, even if `Size` is originally 0.
      bool AlignFixup = false;
      if (F.hasAlignEmitNops()) {
        AlignFixup = getBackend().relaxAlign(F, Size);
        // If the backend does not handle the fragment specially, pad with nops,
        // but ensure that the padding is larger than the minimum nop size.
        if (!AlignFixup)
          while (Size % getBackend().getMinimumNopSize())
            Size += F.getAlignment().value();
      }
      if (!AlignFixup && Size > F.getAlignMaxBytesToEmit())
        Size = 0;
      // Update the variable tail size, offset by FixedSize to prevent ubsan
      // pointer-overflow in evaluateFixup. The content is ignored.
      F.VarContentStart = F.getFixedSize();
      F.VarContentEnd = F.VarContentStart + Size;
      if (F.VarContentEnd > F.getParent()->ContentStorage.size())
        F.getParent()->ContentStorage.resize(F.VarContentEnd);
      Offset += Size;
    } else {
      Offset += computeFragmentSize(F);
    }
  }
}

unsigned MCAssembler::relaxOnce(unsigned FirstStable) {
  ++stats::RelaxationSteps;
  PendingErrors.clear();

  unsigned Res = 0;
  for (unsigned I = 0; I != FirstStable; ++I) {
    // Assume each iteration finalizes at least one extra fragment. If the
    // relaxation does not converge after N+1 iterations, bail out.
    auto &Sec = *Sections[I];
    if (!Sec.curFragList()->Tail)
      continue;
    auto MaxIter = Sec.curFragList()->Tail->getLayoutOrder() + 1;
    for (;;) {
      bool Changed = false;
      for (MCFragment &F : Sec)
        if (F.getKind() != MCFragment::FT_Data && relaxFragment(F))
          Changed = true;

      if (!Changed)
        break;
      // If any fragment changed size, it might impact the layout of subsequent
      // sections. Therefore, we must re-evaluate all sections.
      FirstStable = Sections.size();
      Res = I;
      if (--MaxIter == 0)
        break;
      layoutSection(Sec);
    }
  }
  // The subsequent relaxOnce call only needs to visit Sections [0,Res) if no
  // change occurred.
  return Res;
}

bool MCAssembler::prepareAtfieldAnchors() {
  SmallVector<AtfieldFunctionView, 8> Functions;
  AtfieldFunctionView *Current = nullptr;
  for (MCSection &Section : *this) {
    if (Current) {
      reportError(SMLoc(), "ATField function is split across sections");
      return false;
    }
    for (MCFragment &Fragment : Section) {
      auto *Atfield = dyn_cast<MCAtfieldFragment>(&Fragment);
      if (!Atfield)
        continue;
      if (Atfield->isFunctionBegin()) {
        if (Current) {
          reportError(SMLoc(), "nested ATField function markers");
          return false;
        }
        Functions.push_back({});
        Current = &Functions.back();
        Current->Begin = Atfield;
      } else if (Atfield->isFunctionEnd()) {
        if (!Current) {
          reportError(SMLoc(), "ATField function end has no begin");
          return false;
        }
        Current->End = Atfield;
        Current = nullptr;
      } else if (Atfield->isAnchor()) {
        if (!Current) {
          reportError(SMLoc(), "ATField unit marker is outside a function");
          return false;
        }
        Current->Units.push_back(Atfield);
      }
    }
  }
  if (Current) {
    reportError(SMLoc(), "ATField function has no end marker");
    return false;
  }

  bool Changed = false;
  for (AtfieldFunctionView &Function : Functions) {
    if (!Function.Begin || !Function.End ||
        Function.Begin->getParent() != Function.End->getParent()) {
      reportError(SMLoc(), "ATField function has non-contiguous section layout");
      return false;
    }
    const uint64_t Begin = getFragmentOffset(*Function.Begin);
    const uint64_t End = getFragmentOffset(*Function.End);
    if (End <= Begin) {
      reportError(SMLoc(), "ATField function has zero length");
      return false;
    }

    SmallVector<uint64_t, 16> Natural;
    for (MCFragment &Fragment : *Function.Begin->getParent()) {
      const uint64_t FragmentOffset = getFragmentOffset(Fragment);
      if (FragmentOffset < Begin || FragmentOffset >= End ||
          isa<MCAtfieldFragment>(Fragment))
        continue;
      SmallVector<char, 64> Contents;
      if (!atfieldMaterialize(*this, Fragment, Contents))
        return false;
      if (Contents.empty())
        continue;
      if (atfieldFixupRewritesInstruction(*this, Fragment))
        continue;
      for (uint64_t Offset = 0; Offset != Contents.size(); ++Offset)
        if (static_cast<unsigned char>(Contents[Offset]) == 0xc3 &&
            !atfieldFixupCovers(*this, Fragment, Offset))
          Natural.push_back(FragmentOffset + Offset);
    }
    llvm::sort(Natural);
    Natural.erase(std::unique(Natural.begin(), Natural.end()), Natural.end());

    SmallVector<uint64_t, 16> Stable;
    for (MCAtfieldFragment *Unit : Function.Units)
      if (Unit->isEnabled())
        Stable.push_back(getFragmentOffset(*Unit) + 3);
    Stable.append(Natural.begin(), Natural.end());
    llvm::sort(Stable);
    Stable.erase(std::unique(Stable.begin(), Stable.end()), Stable.end());

    uint64_t Last = Begin;
    bool First = true;
    size_t StableIndex = 0;
    while (StableIndex != Stable.size()) {
      const uint64_t Next = Stable[StableIndex];
      const bool Good = First ? Next - Begin < AtfieldPageSize
                              : Next - Last <= AtfieldPageSize;
      if (Good) {
        Last = Next;
        First = false;
        ++StableIndex;
        continue;
      }
      MCAtfieldFragment *Selected = nullptr;
      uint64_t SelectedOffset = 0;
      for (MCAtfieldFragment *Candidate : Function.Units) {
        if (Candidate->isEnabled())
          continue;
        const uint64_t Offset = getFragmentOffset(*Candidate);
        if (Offset < Last || Offset >= Next || Offset + 3 - Last > AtfieldPageSize ||
            (First && Offset + 3 - Begin >= AtfieldPageSize))
          continue;
        if (!Selected || Offset > SelectedOffset) {
          Selected = Candidate;
          SelectedOffset = Offset;
        }
      }
      if (!Selected) {
        reportError(SMLoc(),
                    "ATField function has no complete-unit anchor boundary");
        return Changed;
      }
      Selected->setEnabled();
      Changed = true;
      Last = SelectedOffset + 3;
      First = false;
    }
    while (End > Last && End - 1 - Last >= AtfieldPageSize) {
      MCAtfieldFragment *Selected = nullptr;
      uint64_t SelectedOffset = 0;
      for (MCAtfieldFragment *Candidate : Function.Units) {
        if (Candidate->isEnabled())
          continue;
        const uint64_t Offset = getFragmentOffset(*Candidate);
        if (Offset < Last || Offset + 3 >= End ||
            Offset + 3 - Last > AtfieldPageSize)
          continue;
        if (!Selected || Offset > SelectedOffset) {
          Selected = Candidate;
          SelectedOffset = Offset;
        }
      }
      if (!Selected) {
        reportError(SMLoc(), "ATField function tail has no anchor boundary");
        return Changed;
      }
      Selected->setEnabled();
      Changed = true;
      Last = SelectedOffset + 3;
    }
    if (End - 1 < Last || End - 1 - Last >= AtfieldPageSize) {
      reportError(SMLoc(), "ATField function anchor interval is too long");
      return Changed;
    }
  }
  return Changed;
}

void MCAssembler::emitAtfieldSymbols() {
  AtfieldManifestRecords.clear();
  SmallVector<AtfieldFunctionView, 8> Functions;
  AtfieldFunctionView *Current = nullptr;
  for (MCSection &Section : *this)
    for (MCFragment &Fragment : Section) {
      auto *Atfield = dyn_cast<MCAtfieldFragment>(&Fragment);
      if (!Atfield)
        continue;
      if (Atfield->isFunctionBegin()) {
        Functions.push_back({});
        Current = &Functions.back();
        Current->Begin = Atfield;
      } else if (Atfield->isFunctionEnd()) {
        if (Current) {
          Current->End = Atfield;
          Current = nullptr;
        }
      } else if (Current && Atfield->isAnchor())
        Current->Units.push_back(Atfield);
    }

  for (AtfieldFunctionView &Function : Functions) {
    if (!Function.Begin || !Function.End ||
        Function.Begin->getParent() != Function.End->getParent()) {
      reportError(SMLoc(), "ATField function has incomplete anchor range");
      return;
    }
    SmallVector<std::pair<uint64_t, MCAtfieldFragment *>, 16> Locations;
    for (MCAtfieldFragment *Unit : Function.Units)
      if (Unit->isEnabled())
        Locations.push_back({getFragmentOffset(*Unit) + 3, Unit});
    const uint64_t Begin = getFragmentOffset(*Function.Begin);
    const uint64_t End = getFragmentOffset(*Function.End);
    auto EmitFunctionSymbol = [&](StringRef Name, MCFragment *Target) {
      MCSymbol *FunctionSymbol =
          getContext().getOrCreateSymbol(Name);
      if (FunctionSymbol->isDefined()) {
        reportError(SMLoc(), "duplicate ATField function symbol");
        return false;
      }
      FunctionSymbol->setFragment(Target);
      FunctionSymbol->setOffset(0);
      registerSymbol(*FunctionSymbol);
      auto *ELFSymbol = static_cast<MCSymbolELF *>(FunctionSymbol);
      ELFSymbol->setBinding(ELF::STB_GLOBAL);
      ELFSymbol->setVisibility(ELF::STV_HIDDEN);
      ELFSymbol->setType(ELF::STT_NOTYPE);
      return true;
    };
    const std::string BeginName =
        (Twine("__atfield_function_begin_p") +
         Twine(Function.Begin->getPayloadOrdinal()) + Twine("_f") +
         Twine(Function.Begin->getFunctionOrdinal()))
            .str();
    const std::string EndName =
        (Twine("__atfield_function_end_p") +
         Twine(Function.Begin->getPayloadOrdinal()) + Twine("_f") +
         Twine(Function.Begin->getFunctionOrdinal()))
            .str();
    if (!EmitFunctionSymbol(BeginName, Function.Begin) ||
        !EmitFunctionSymbol(EndName, Function.End))
      return;
    AtfieldManifestRecords.push_back(
        {3, 0, 0, Function.Begin->getPayloadOrdinal(),
         Function.Begin->getFunctionOrdinal(), 0, Function.Begin, 0, Begin,
         End});
    SmallVector<std::pair<uint64_t, uint64_t>, 16> UnitRanges;
    uint64_t SerializedUnitOrdinal = 0;
    for (size_t UnitIndex = 0; UnitIndex != Function.Units.size();
         ++UnitIndex) {
      MCAtfieldFragment *Unit = Function.Units[UnitIndex];
      const uint64_t UnitBegin = getFragmentOffset(*Unit);
      const uint64_t SerializedUnitBegin =
          Unit->isEnabled() ? UnitBegin + 7 : UnitBegin;
      const uint64_t UnitEnd =
          UnitIndex + 1 == Function.Units.size()
              ? End
              : getFragmentOffset(*Function.Units[UnitIndex + 1]);
      if (UnitEnd < SerializedUnitBegin) {
        reportError(SMLoc(), "ATField unit range is not monotonic");
        return;
      }
      if (UnitEnd > SerializedUnitBegin) {
        AtfieldManifestRecords.push_back(
            {2, Unit->getUnitKind(), static_cast<uint8_t>(Unit->isBundle()),
             Function.Begin->getPayloadOrdinal(),
             Function.Begin->getFunctionOrdinal(), SerializedUnitOrdinal++, Unit,
             0, SerializedUnitBegin, UnitEnd});
        UnitRanges.push_back({SerializedUnitBegin, UnitEnd});
      }
    }
    uint64_t ConstructedOrdinal = SerializedUnitOrdinal;
    for (MCAtfieldFragment *Unit : Function.Units) {
      if (!Unit->isEnabled())
        continue;
      const uint64_t AnchorBegin = getFragmentOffset(*Unit);
      UnitRanges.push_back({AnchorBegin, AnchorBegin + 7});
      AtfieldManifestRecords.push_back(
          {2, 4, 0, Function.Begin->getPayloadOrdinal(),
           Function.Begin->getFunctionOrdinal(), ConstructedOrdinal++, Unit, 0,
           AnchorBegin, AnchorBegin + 7});
    }
    llvm::sort(UnitRanges);
    uint64_t UnitCursor = Begin;
    for (auto [UnitBegin, UnitEnd] : UnitRanges) {
      if (UnitBegin != UnitCursor || UnitEnd < UnitBegin) {
        reportError(SMLoc(), "ATField unit ranges are not continuous");
        return;
      }
      UnitCursor = UnitEnd;
    }
    if (UnitCursor != End) {
      reportError(SMLoc(), "ATField unit ranges do not cover function");
      return;
    }
    for (MCFragment &Fragment : *Function.Begin->getParent()) {
      const uint64_t FragmentOffset = getFragmentOffset(Fragment);
      if (FragmentOffset < Begin || FragmentOffset >= End ||
          isa<MCAtfieldFragment>(Fragment))
        continue;
      SmallVector<char, 64> Contents;
      if (!atfieldMaterialize(*this, Fragment, Contents) || Contents.empty() ||
          atfieldFixupRewritesInstruction(*this, Fragment))
        continue;
      for (uint64_t Offset = 0; Offset != Contents.size(); ++Offset)
        if (static_cast<unsigned char>(Contents[Offset]) == 0xc3 &&
            !atfieldFixupCovers(*this, Fragment, Offset))
          Locations.push_back({FragmentOffset + Offset, nullptr});
    }
    llvm::sort(Locations, [](auto &L, auto &R) { return L.first < R.first; });
    Locations.erase(std::unique(Locations.begin(), Locations.end(),
                                [](auto &L, auto &R) { return L.first == R.first; }),
                    Locations.end());

    uint64_t AnchorOrdinal = 0;
    for (auto [Address, Fragment] : Locations) {
      MCFragment *Target = Fragment;
      uint64_t Offset = 3;
      if (!Target) {
        for (MCFragment &Candidate : *Function.Begin->getParent()) {
          SmallVector<char, 64> Contents;
          if (!atfieldMaterialize(*this, Candidate, Contents))
            continue;
          const uint64_t CandidateOffset = getFragmentOffset(Candidate);
          if (Address < CandidateOffset || Address >= CandidateOffset + Contents.size())
            continue;
          Target = &Candidate;
          Offset = Address - CandidateOffset;
          break;
        }
      }
      if (!Target) {
        reportError(SMLoc(), "ATField anchor has no materialized target");
        return;
      }
      const std::string Name =
          (Twine("__atfield_anchor_p") +
           Twine(Function.Begin->getPayloadOrdinal()) + Twine("_f") +
           Twine(Function.Begin->getFunctionOrdinal()) + Twine("_a") +
           Twine(AnchorOrdinal++))
              .str();
      MCSymbol *Symbol = getContext().getOrCreateSymbol(Name);
      if (Symbol->isDefined()) {
        reportError(SMLoc(), "duplicate ATField anchor symbol");
        return;
      }
      Symbol->setFragment(Target);
      Symbol->setOffset(Offset);
      registerSymbol(*Symbol);
      auto *ELFSymbol = static_cast<MCSymbolELF *>(Symbol);
      ELFSymbol->setBinding(ELF::STB_GLOBAL);
      ELFSymbol->setVisibility(ELF::STV_HIDDEN);
      ELFSymbol->setType(ELF::STT_NOTYPE);
      AtfieldManifestRecords.push_back(
          {1,
           static_cast<uint8_t>(Fragment ? 2 : 1),
           0,
           Function.Begin->getPayloadOrdinal(),
           Function.Begin->getFunctionOrdinal(),
           AnchorOrdinal - 1,
           Target,
           Offset,
           Begin,
           End});
    }
  }
}

void MCAssembler::emitAtfieldManifest() {
  if (!AtfieldManifestSection)
    return;

  struct Record {
    uint8_t Tag = 0;
    uint8_t Kind = 0;
    uint8_t Bundle = 0;
    uint64_t Payload = 0;
    uint64_t Function = 0;
    uint64_t Ordinal = 0;
    uint64_t Offset = 0;
    uint64_t Begin = 0;
    uint64_t End = 0;
  };

  SmallVector<Record, 16> Records;
  for (const AtfieldManifestRecord &Entry : AtfieldManifestRecords) {
    if (!Entry.Fragment || !Entry.Fragment->getParent() || Entry.End < Entry.Begin) {
      reportError(SMLoc(), "invalid ATField anchor manifest record");
      return;
    }
    const uint64_t Offset =
        Entry.Tag == 1 ? getFragmentOffset(*Entry.Fragment) + Entry.Offset : 0;
    Records.push_back({Entry.Tag,
                       Entry.Kind,
                       Entry.Bundle,
                       Entry.Payload,
                       Entry.Function,
                       Entry.Ordinal,
                       Offset,
                       Entry.Begin,
                       Entry.End});
  }
  llvm::sort(Records, [](const Record &Left, const Record &Right) {
    return std::tie(Left.Payload, Left.Function, Left.Tag, Left.Ordinal) <
           std::tie(Right.Payload, Right.Function, Right.Tag, Right.Ordinal);
  });
  if (Records.empty()) {
    reportError(SMLoc(), "ATField manifest has no anchor records");
    return;
  }
  if (Records.size() > std::numeric_limits<uint32_t>::max()) {
    reportError(SMLoc(), "ATField manifest has too many anchor records");
    return;
  }

  SmallVector<char, 0> Description;
  auto append16 = [&](uint16_t Value) {
    Description.push_back(static_cast<char>(Value));
    Description.push_back(static_cast<char>(Value >> 8));
  };
  auto append32 = [&](uint32_t Value) {
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Description.push_back(static_cast<char>(Value >> Shift));
  };
  auto append64 = [&](uint64_t Value) {
    for (unsigned Shift = 0; Shift != 64; Shift += 8)
      Description.push_back(static_cast<char>(Value >> Shift));
  };
  append32(0x324e4641u);
  append16(1);
  append16(80);
  append32(static_cast<uint32_t>(Records.size()));
  append32(0);
  for (const Record &Entry : Records) {
    Description.push_back(static_cast<char>(Entry.Tag));
    Description.push_back(static_cast<char>(Entry.Kind));
    Description.push_back(static_cast<char>(Entry.Bundle));
    Description.push_back(0);
    append32(0);
    append64(Entry.Payload);
    append64(Entry.Function);
    append64(Entry.Ordinal);
    append32(0);
    append32(0);
    append64(Entry.Begin);
    append64(Entry.End);
    append64(Entry.Offset);
    append64(0);
    append64(0);
  }
  assert(Description.size() == 16 + Records.size() * 80 &&
         "invalid AFN2 record serialization");

  SmallVector<char, 0> Note;
  auto appendNote32 = [&](uint32_t Value) {
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Note.push_back(static_cast<char>(Value >> Shift));
  };
  appendNote32(8);
  appendNote32(static_cast<uint32_t>(Description.size()));
  appendNote32(0x41544641u);
  Note.append({'A', 'T', 'F', 'i', 'e', 'l', 'd', '\0'});
  Note.append(Description.begin(), Description.end());
  while (Note.size() % 4)
    Note.push_back(0);

  auto Storage = std::make_unique<char[]>(sizeof(MCFragment) + Note.size());
  auto *Fragment = new (Storage.get()) MCFragment(MCFragment::FT_Data);
  Fragment->setParent(AtfieldManifestSection);
  Fragment->FixedSize = Note.size();
  std::memcpy(Fragment + 1, Note.data(), Note.size());
  AtfieldManifestStorage.push_back(std::move(Storage));
  MCSection::FragList *List = AtfieldManifestSection->curFragList();
  if (List->Tail)
    List->Tail->Next = Fragment;
  else
    List->Head = Fragment;
  List->Tail = Fragment;
}

void MCAssembler::reportError(SMLoc L, const Twine &Msg) const {
  getContext().reportError(L, Msg);
}

void MCAssembler::recordError(SMLoc Loc, const Twine &Msg) const {
  PendingErrors.emplace_back(Loc, Msg.str());
}

void MCAssembler::flushPendingErrors() const {
  for (auto &Err : PendingErrors)
    reportError(Err.first, Err.second);
  PendingErrors.clear();
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void MCAssembler::dump() const{
  raw_ostream &OS = errs();
  DenseMap<const MCFragment *, SmallVector<const MCSymbol *, 0>> FragToSyms;
  // Scan symbols and build a map of fragments to their corresponding symbols.
  // For variable symbols, we don't want to call their getFragment, which might
  // modify `Fragment`.
  for (const MCSymbol &Sym : symbols())
    if (!Sym.isVariable())
      if (auto *F = Sym.getFragment())
        FragToSyms.try_emplace(F).first->second.push_back(&Sym);

  OS << "Sections:[";
  for (const MCSection &Sec : *this) {
    OS << '\n';
    Sec.dump(&FragToSyms);
  }
  OS << "\n]\n";
}
#endif

SMLoc MCFixup::getLoc() const {
  if (auto *E = getValue())
    return E->getLoc();
  return {};
}
