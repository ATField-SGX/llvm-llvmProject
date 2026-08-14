//===- ATFieldInputObserver.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/ELF/ATFieldInputObserver.h"
#include "InputFiles.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

using namespace lld::elf;
using namespace llvm;

namespace {
ATFieldObserverState &state() { return lld::context<Ctx>().atfieldObserver; }
} // namespace

ATFieldInputObserver *lld::elf::setATFieldInputObserver(
    ATFieldInputObserver *value) noexcept {
  auto &s = state();
  ATFieldInputObserver *previous = s.observer;
  s.observer = value;
  return previous;
}
ATFieldInputObserver *lld::elf::getATFieldInputObserver() noexcept {
  return state().observer;
}
ATFieldPreparedInputProvider *lld::elf::setATFieldPreparedInputProvider(
    ATFieldPreparedInputProvider *value) noexcept {
  auto &s = state();
  ATFieldPreparedInputProvider *previous = s.preparedInputProvider;
  s.preparedInputProvider = value;
  return previous;
}
ATFieldPreparedInputProvider *
lld::elf::getATFieldPreparedInputProvider() noexcept {
  return state().preparedInputProvider;
}
uint64_t lld::elf::translateATFieldArgumentOrdinal(
    uint64_t currentOrdinal) noexcept {
  auto *provider = getATFieldPreparedInputProvider();
  if (!provider)
    return currentOrdinal;
  llvm::Expected<uint64_t> translated =
      provider->translateArgumentOrdinal(currentOrdinal);
  if (!translated)
    fatal("prepared input ordinal translation failed: " +
          toString(translated.takeError()));
  return *translated;
}
uint64_t lld::elf::translateATFieldArchiveOccurrence(
    uint64_t currentOccurrence, uint64_t argumentOrdinal,
    uint64_t encounterOrdinal) noexcept {
  auto *provider = getATFieldPreparedInputProvider();
  if (!provider)
    return currentOccurrence;
  llvm::Expected<uint64_t> translated =
      provider->translateArchiveOccurrence(currentOccurrence, argumentOrdinal,
                                           encounterOrdinal);
  if (!translated)
    fatal("prepared input archive occurrence translation failed: " +
          toString(translated.takeError()));
  return *translated;
}
uint64_t lld::elf::translateATFieldArchiveMemberOccurrence(
    uint64_t currentOccurrence, uint64_t archiveOccurrence,
    uint64_t argumentOrdinal, uint64_t childHeaderOffset,
    uint64_t memberOrdinal, bool thinArchive) noexcept {
  auto *provider = getATFieldPreparedInputProvider();
  if (!provider)
    return currentOccurrence;
  llvm::Expected<uint64_t> translated =
      provider->translateArchiveMemberOccurrence(
          currentOccurrence, archiveOccurrence, argumentOrdinal,
          childHeaderOffset, memberOrdinal, thinArchive);
  if (!translated)
    fatal("prepared input member occurrence translation failed: " +
          toString(translated.takeError()));
  return *translated;
}
uint64_t lld::elf::nextATFieldArchiveEncounterOrdinal(
    uint64_t argumentOrdinal) noexcept {
  return state().archiveEncounterOrdinals[argumentOrdinal]++;
}
ATFieldOccurrence lld::elf::nextATFieldOccurrence() noexcept {
  return state().nextOccurrence++;
}
uint64_t lld::elf::nextATFieldPayloadOrdinal() noexcept {
  return state().payloadOrdinal++;
}
void lld::elf::beginATFieldLink() noexcept {
  auto &s = state();
  s.nextOccurrence = 1;
  s.payloadOrdinal = 0;
  s.argumentContext = {};
  s.candidate.clear();
  s.winners.clear();
  s.canonicalTargetTokens.clear();
  s.nextCanonicalTargetToken = 1;
  s.payloadIncludedEvents.clear();
  s.preparedInputSections.clear();
  s.preparedInputSectionPieces.clear();
  s.preparedInputSectionStrings.clear();
  s.scriptOccurrences.clear();
  s.archiveEncounterOrdinals.clear();
  s.payloadSnapshotNotified = false;
}
ATFieldOccurrence lld::elf::ensureATFieldScriptOccurrence(
    uint64_t argumentOrdinal) noexcept {
  auto &s = state();
  if (auto it = s.scriptOccurrences.find(argumentOrdinal);
      it != s.scriptOccurrences.end())
    return it->second;
  ATFieldOccurrence occurrence = nextATFieldOccurrence();
  if (auto *provider = getATFieldPreparedInputProvider()) {
    llvm::Expected<uint64_t> translated =
        provider->translateLinkerScriptOccurrence(occurrence, argumentOrdinal);
    if (!translated)
      fatal("prepared linker script occurrence translation failed: " +
            toString(translated.takeError()));
    occurrence = *translated;
  }
  s.scriptOccurrences[argumentOrdinal] = occurrence;
  return occurrence;
}
ATFieldArgumentContext lld::elf::getATFieldArgumentContext() noexcept {
  return state().argumentContext;
}
void lld::elf::setATFieldArgumentContext(
    const ATFieldArgumentContext &value) noexcept {
  state().argumentContext = value;
}
void lld::elf::clearATFieldArgumentContext() noexcept {
  state().argumentContext = {};
}

static bool isATFieldMetadataType(uint32_t type) {
  return type == llvm::ELF::SHT_NULL || type == llvm::ELF::SHT_SYMTAB ||
         type == llvm::ELF::SHT_STRTAB || type == llvm::ELF::SHT_REL ||
         type == llvm::ELF::SHT_RELA || type == llvm::ELF::SHT_CREL ||
         type == llvm::ELF::SHT_SYMTAB_SHNDX || type == llvm::ELF::SHT_GROUP ||
         type == llvm::ELF::SHT_LLVM_ADDRSIG ||
         type == llvm::ELF::SHT_LLVM_CALL_GRAPH_PROFILE ||
         type == llvm::ELF::SHT_LLVM_DEPENDENT_LIBRARIES ||
         type == llvm::ELF::SHT_GNU_ATTRIBUTES ||
         type == llvm::ELF::SHT_ARM_ATTRIBUTES ||
         type == llvm::ELF::SHT_AARCH64_ATTRIBUTES ||
         type == llvm::ELF::SHT_RISCV_ATTRIBUTES ||
         type == llvm::ELF::SHT_MIPS_ABIFLAGS;
}

static StringRef ownATFieldSectionString(ATFieldObserverState &s,
                                         StringRef value) {
  auto storage = std::make_unique<std::string>(value.str());
  StringRef result(*storage);
  s.preparedInputSectionStrings.push_back(std::move(storage));
  return result;
}

static void setATFieldSectionPlacement(
    Ctx &ctx, InputSectionBase *section,
    ATFieldInputSectionResolutionEvent &event) {
  if (ctx.atfieldExplicitSections.contains(section))
    event.placement = ATFieldInputSectionPlacement::Explicit;
  else if (ctx.atfieldOrphanSections.contains(section))
    event.placement = ATFieldInputSectionPlacement::Orphan;
}

static void appendATFieldSectionPiece(
    SmallVectorImpl<ATFieldInputSectionPiece> &pieces, uint64_t inputOffset,
    uint64_t size, uint64_t outputOffset,
    ATFieldInputSectionPieceDisposition disposition, bool live) {
  if (size == 0)
    return;
  ATFieldInputSectionPiece &piece = pieces.emplace_back();
  piece.inputOffset = inputOffset;
  piece.size = size;
  piece.outputOffset = outputOffset;
  piece.disposition = disposition;
  piece.live = live;
}

static void fillATFieldPieceTarget(ATFieldInputSectionPiece &piece,
                                   Symbol *target) {
  if (!target)
    return;
  piece.targetDefined = target->isDefined();
  piece.targetFolded = target->folded;
  piece.targetPartition = target->partition;
  if (target->isDefined())
    piece.targetHasSection = cast<Defined>(target)->section != nullptr;
  piece.resolvedTargetHasWinner = getATFieldSymbolWinner(
      target, piece.resolvedTargetInputOccurrence,
      piece.resolvedTargetInputSymbolIndex,
      piece.resolvedTargetInputSectionIndex);
}

static bool fillATFieldMergePieces(
    Ctx &ctx, MergeInputSection &section,
    SmallVectorImpl<ATFieldInputSectionPiece> &pieces) {
  uint64_t cursor = 0;
  uint64_t contentSize = section.content().size();
  for (size_t index = 0; index != section.pieces.size(); ++index) {
    const SectionPiece &source = section.pieces[index];
    uint64_t inputOffset = source.inputOff;
    uint64_t end = index + 1 == section.pieces.size()
                       ? contentSize
                       : section.pieces[index + 1].inputOff;
    if (inputOffset != cursor || end < inputOffset || end > contentSize) {
      ErrAlways(ctx) << "ATField merge section pieces do not cover input";
      return false;
    }
    bool retained = source.live && section.getOutputSection();
    appendATFieldSectionPiece(
        pieces, inputOffset, end - inputOffset,
        retained ? section.getOffset(inputOffset) : ~uint64_t(0),
        retained ? ATFieldInputSectionPieceDisposition::Retained
                 : ATFieldInputSectionPieceDisposition::Dead,
        retained);
    cursor = end;
  }
  if (cursor != contentSize) {
    ErrAlways(ctx) << "ATField merge section pieces do not cover input";
    return false;
  }
  return true;
}

static bool fillATFieldEhPieces(
    Ctx &ctx, EhInputSection &section,
    SmallVectorImpl<ATFieldInputSectionPiece> &pieces) {
  SmallVector<std::pair<const EhSectionPiece *, bool>, 0> sources;
  sources.reserve(section.cies.size() + section.fdes.size());
  for (const EhSectionPiece &piece : section.cies)
    sources.emplace_back(&piece, true);
  for (const EhSectionPiece &piece : section.fdes)
    sources.emplace_back(&piece, false);
  llvm::sort(sources, [](const auto &a, const auto &b) {
    return a.first->inputOff < b.first->inputOff;
  });

  uint64_t cursor = 0;
  uint64_t contentSize = section.content().size();
  for (const auto &[source, cie] : sources) {
    uint64_t inputOffset = source->inputOff;
    uint64_t end = inputOffset + source->size;
    if (inputOffset < cursor || end < inputOffset || end > contentSize) {
      ErrAlways(ctx) << "ATField EH section pieces overlap or exceed input";
      return false;
    }
    if (inputOffset > cursor) {
      if (!llvm::all_of(
              section.content().slice(cursor, inputOffset - cursor),
              [](uint8_t byte) { return byte == 0; })) {
        ErrAlways(ctx) << "ATField EH gap contains non-zero bytes";
        return false;
      }
      appendATFieldSectionPiece(
          pieces, cursor, inputOffset - cursor, ~uint64_t(0),
          ATFieldInputSectionPieceDisposition::Padding, false);
    }

    bool zeroTerminator =
        source->size == 4 &&
        llvm::all_of(section.content().slice(inputOffset, source->size),
                     [](uint8_t byte) { return byte == 0; });
    bool retained = section.isLive() && source->outputOff >= 0 &&
                    section.getOutputSection() && !zeroTerminator;
    appendATFieldSectionPiece(
        pieces, inputOffset, source->size,
        retained ? section.getOffset(inputOffset) : ~uint64_t(0),
        zeroTerminator
            ? ATFieldInputSectionPieceDisposition::Padding
            : retained ? ATFieldInputSectionPieceDisposition::Retained
                       : ATFieldInputSectionPieceDisposition::Dead,
        retained);
    ATFieldInputSectionPiece &record = pieces.back();
    if (!zeroTerminator) {
      if (!cie && source->firstRelocation == unsigned(-1)) {
        ErrAlways(ctx) << "ATField EH FDE lacks relocation provenance";
        return false;
      }
      record.cie = cie;
      record.firstRelocationIndex = source->rawRelocationIndex;
      record.rawTargetInputSymbolIndex = source->rawSymbolIndex;
      Symbol *target = source->firstRelocation != unsigned(-1) &&
                               source->firstRelocation < section.rels.size()
                           ? section.rels[source->firstRelocation].sym
                           : nullptr;
      fillATFieldPieceTarget(record, target);
    }
    cursor = end;
  }
  if (cursor < contentSize) {
    if (!llvm::all_of(
            section.content().slice(cursor, contentSize - cursor),
            [](uint8_t byte) { return byte == 0; })) {
      ErrAlways(ctx) << "ATField EH trailing padding contains non-zero bytes";
      return false;
    }
    if (cursor + 4 <= contentSize) {
      appendATFieldSectionPiece(
          pieces, cursor, 4, ~uint64_t(0),
          ATFieldInputSectionPieceDisposition::Padding, false);
      cursor += 4;
    }
  }
  if (cursor < contentSize)
    appendATFieldSectionPiece(
        pieces, cursor, contentSize - cursor, ~uint64_t(0),
        ATFieldInputSectionPieceDisposition::Padding, false);
  return true;
}

void lld::elf::prepareATFieldInputSections(Ctx &ctx) noexcept {
  if (!getATFieldInputObserver())
    return;
  auto &s = state();
  s.preparedInputSections.clear();
  s.preparedInputSectionPieces.clear();
  s.preparedInputSectionStrings.clear();
  SmallVector<std::pair<size_t, size_t>, 0> pieceRanges;

  for (ELFFileBase *file : ctx.objectFiles) {
    if (!file->atfieldIncluded || file->kind() != InputFile::ObjKind)
      continue;
    ArrayRef<InputSectionBase *> sections = file->getSections();
    if (file->atfieldSectionSnapshots.size() != sections.size()) {
      ErrAlways(ctx) << "ATField section snapshot does not match input";
      continue;
    }
    for (uint32_t index = 0; index != sections.size(); ++index) {
      InputSectionBase *section = sections[index];
      const ATFieldInputSectionSnapshot &snapshot =
          file->atfieldSectionSnapshots[index];
      ATFieldInputSectionResolutionEvent event;
      event.inputOccurrence = file->atfieldInputOccurrence;
      event.argumentOrdinal = file->atfieldArgumentOrdinal;
      event.groupId = file->atfieldGroupId;
      event.inputSectionIndex = index;
      event.present = snapshot.present;
      event.inputSectionName = ownATFieldSectionString(s, snapshot.name);
      event.inputSectionType = snapshot.type;
      event.inputSectionFlags = snapshot.flags;
      event.inputSectionSize = snapshot.size;
      event.discardReason = snapshot.discardReason;
      setATFieldSectionPlacement(ctx, section, event);

      bool knownMetadata = isATFieldMetadataType(snapshot.type);
      bool forcedDiscard =
          snapshot.discarded &&
          snapshot.discardReason != ATFieldInputSectionDiscardReason::None &&
          (!knownMetadata ||
           snapshot.discardReason != ATFieldInputSectionDiscardReason::Parser);
      if (section && section == &InputSection::discarded) {
        forcedDiscard = !knownMetadata ||
                        snapshot.discardReason !=
                            ATFieldInputSectionDiscardReason::Parser;
        if (event.discardReason ==
            ATFieldInputSectionDiscardReason::None)
          event.discardReason = ATFieldInputSectionDiscardReason::Parser;
      }
      if (section && ctx.atfieldScriptDiscardedSections.contains(section)) {
        forcedDiscard = true;
        event.discardReason = ATFieldInputSectionDiscardReason::Script;
      }

      if (forcedDiscard) {
        event.disposition = ATFieldInputSectionDisposition::Discarded;
        event.discarded = true;
      } else if (knownMetadata &&
                 (!section || section == &InputSection::discarded ||
                  !section->isLive() || !section->getOutputSection())) {
        event.disposition = ATFieldInputSectionDisposition::Metadata;
      } else if (!section) {
        ErrAlways(ctx) << "ATField section has no unique disposition";
        event.disposition = ATFieldInputSectionDisposition::Metadata;
      } else if (!section->isLive() || !section->getOutputSection()) {
        event.disposition = ATFieldInputSectionDisposition::Dead;
        event.live = section->isLive();
      } else {
        event.disposition = ATFieldInputSectionDisposition::Retained;
        event.live = true;
      }
      if (event.disposition == ATFieldInputSectionDisposition::Metadata)
        event.discardReason = ATFieldInputSectionDiscardReason::None;
      if (event.disposition == ATFieldInputSectionDisposition::Retained &&
          isa<EhInputSection>(section))
        event.placement = ATFieldInputSectionPlacement::Synthetic;
      else if (event.disposition != ATFieldInputSectionDisposition::Retained)
        event.placement = ATFieldInputSectionPlacement::None;

      if (event.disposition == ATFieldInputSectionDisposition::Retained &&
          section) {
        OutputSection *output = section->getOutputSection();
        event.hasOutputSection = true;
        event.outputSectionIndex = output->sectionIndex;
        event.outputSectionName = ownATFieldSectionString(s, output->name);
        event.outputSectionType = output->type;
        event.outputSectionFlags = output->flags;
        event.outputSectionVA = output->addr;
        event.outputSectionFileOffset = output->offset;
        event.outputSectionSize = output->size;
        event.outputSectionAlignment = output->addralign;
        event.outputSectionOffset = section->getOffset(0);
      }

      size_t pieceStart = s.preparedInputSectionPieces.size();
      bool piecesValid = true;
      if (event.disposition != ATFieldInputSectionDisposition::Discarded &&
          section) {
        if (auto *merge = dyn_cast<MergeInputSection>(section))
          piecesValid =
              fillATFieldMergePieces(ctx, *merge, s.preparedInputSectionPieces);
        else if (auto *eh = dyn_cast<EhInputSection>(section))
          piecesValid =
              fillATFieldEhPieces(ctx, *eh, s.preparedInputSectionPieces);
      }
      if (!piecesValid)
        event.disposition = ATFieldInputSectionDisposition::Dead;
      size_t pieceCount =
          s.preparedInputSectionPieces.size() - pieceStart;
      s.preparedInputSections.push_back(event);
      pieceRanges.emplace_back(pieceStart, pieceCount);
    }
  }
  for (size_t i = 0; i != s.preparedInputSections.size(); ++i) {
    auto [start, count] = pieceRanges[i];
    s.preparedInputSections[i].pieces =
        ArrayRef<ATFieldInputSectionPiece>(
            s.preparedInputSectionPieces)
            .slice(start, count);
  }
}

void lld::elf::notifyATFieldInputSections(Ctx &ctx) noexcept {
  (void)ctx;
  auto *observer = getATFieldInputObserver();
  if (!observer)
    return;
  observer->onInputSectionsResolved(state().preparedInputSections);
}

lld::elf::ATFieldSymbolCandidateScope::ATFieldSymbolCandidateScope(
    Symbol *symbol, InputFile *file, uint64_t inputSymbolIndex,
    uint32_t inputSectionIndex, uint8_t inputBinding, uint8_t inputType,
    uint8_t inputVisibility, bool common, bool weak, bool comdat)
    : symbol(symbol) {
  if (!getATFieldInputObserver() || !symbol || !file)
    return;
  auto &s = state();
  lock = std::unique_lock<std::recursive_mutex>(s.symbolStateMutex);
  s.candidate[symbol] = {file, inputSymbolIndex, inputSectionIndex,
                         inputBinding, inputType, inputVisibility, common,
                         weak, comdat};
}
lld::elf::ATFieldSymbolCandidateScope::~ATFieldSymbolCandidateScope() {
  if (!lock.owns_lock())
    return;
  state().candidate.erase(symbol);
  lock.unlock();
}
void lld::elf::invalidateATFieldSymbolWinner(Symbol *symbol) noexcept {
  if (!getATFieldInputObserver() || !symbol)
    return;
  auto &s = state();
  std::lock_guard<std::recursive_mutex> lock(s.symbolStateMutex);
  s.winners.erase(symbol);
}
void lld::elf::noteATFieldSymbolWinner(Symbol *symbol) noexcept {
  if (!getATFieldInputObserver() || !symbol)
    return;
  auto &s = state();
  std::lock_guard<std::recursive_mutex> lock(s.symbolStateMutex);
  if (auto it = s.candidate.find(symbol); it != s.candidate.end())
    s.winners[symbol] = it->second;
}
static bool isATFieldPublishedWinner(Symbol *symbol) {
  if (!symbol->isDefined())
    return false;
  auto *defined = llvm::cast<Defined>(symbol);
  if (!defined->section)
    return true;
  auto *input = llvm::dyn_cast<InputSectionBase>(defined->section);
  return input && input != &InputSection::discarded && input->isLive() &&
         input->getOutputSection();
}
bool lld::elf::getATFieldSymbolWinner(Symbol *symbol,
                                      ATFieldOccurrence &inputOccurrence,
                                      uint64_t &inputSymbolIndex,
                                      uint32_t &inputSectionIndex) noexcept {
  // Called after parallel symbol initialization joins; terminal reads do not
  // hold symbolStateMutex so observer callbacks never run under this lock.
  auto &s = state();
  auto it = s.winners.find(symbol);
  if (it == s.winners.end() || !it->second.file ||
      !isATFieldPublishedWinner(symbol))
    return false;
  inputOccurrence = it->second.file->atfieldInputOccurrence;
  inputSymbolIndex = it->second.inputSymbolIndex;
  inputSectionIndex = it->second.inputSectionIndex;
  return true;
}
void lld::elf::notifyATFieldSymbolWinners(Ctx &ctx) noexcept {
  // Called after parallel symbol initialization joins. Snapshot all state
  // before callbacks and never hold symbolStateMutex across observer calls.
  auto *current = getATFieldInputObserver();
  if (!current)
    return;
  auto &s = state();
  llvm::SmallVector<Symbol *, 0> symbols;
  llvm::DenseSet<Symbol *> seen;
  for (Symbol *symbol : ctx.symtab->getSymbols())
    if (symbol && seen.insert(symbol).second)
      symbols.push_back(symbol);
  for (ELFFileBase *file : ctx.objectFiles)
    for (Symbol *symbol : file->getSymbols())
      if (symbol && seen.insert(symbol).second)
        symbols.push_back(symbol);
  llvm::SmallVector<ATFieldSymbolWinnerKey, 0> expectedKeys;
  for (Symbol *symbol : symbols) {
    auto it = s.winners.find(symbol);
    if (it == s.winners.end() || !isATFieldPublishedWinner(symbol))
      continue;
    const ATFieldSymbolCandidate &winner = it->second;
    ATFieldSymbolWinnerEvent event;
    event.canonicalName = symbol->getName();
    event.inputOccurrence = winner.file->atfieldInputOccurrence;
    event.archiveOccurrence = winner.file->atfieldArchiveOccurrence;
    event.memberOccurrence = winner.file->atfieldMemberOccurrence;
    event.groupId = winner.file->atfieldGroupId;
    event.inputSymbolIndex = winner.inputSymbolIndex;
    event.inputSectionIndex = winner.inputSectionIndex;
    event.inputBinding = winner.inputBinding;
    event.inputType = winner.inputType;
    event.inputVisibility = winner.inputVisibility;
    if (OutputSection *output = symbol->getOutputSection())
      event.outputSectionIndex = output->sectionIndex;
    event.outputRva = symbol->getVA(ctx);
    event.common = winner.common;
    event.weak = winner.weak;
    event.comdat = winner.comdat;
    current->onSymbolWinner(event);
    expectedKeys.push_back(
        {winner.file->atfieldInputOccurrence, winner.inputSymbolIndex});
  }
  std::sort(expectedKeys.begin(), expectedKeys.end(),
            [](const ATFieldSymbolWinnerKey &a,
               const ATFieldSymbolWinnerKey &b) {
              if (a.inputOccurrence != b.inputOccurrence)
                return a.inputOccurrence < b.inputOccurrence;
              return a.inputSymbolIndex < b.inputSymbolIndex;
            });
  expectedKeys.erase(
      std::unique(expectedKeys.begin(), expectedKeys.end(),
                  [](const ATFieldSymbolWinnerKey &a,
                     const ATFieldSymbolWinnerKey &b) {
                    return a.inputOccurrence == b.inputOccurrence &&
                           a.inputSymbolIndex == b.inputSymbolIndex;
                  }),
      expectedKeys.end());
  current->onExpectedSymbolWinnerKeys(expectedKeys);

  llvm::SmallVector<ATFieldInputSymbolBinding, 0> bindings;
  for (ELFFileBase *file : ctx.objectFiles) {
    ArrayRef<Symbol *> inputSymbols = file->getSymbols();
    for (uint64_t index = 0; index != inputSymbols.size(); ++index) {
      Symbol *target = inputSymbols[index];
      ATFieldInputSymbolBinding binding;
      binding.inputOccurrence = file->atfieldInputOccurrence;
      binding.inputSymbolIndex = index;
      if (target) {
        auto token = s.canonicalTargetTokens.find(target);
        if (token == s.canonicalTargetTokens.end())
          token = s.canonicalTargetTokens
                      .try_emplace(target, s.nextCanonicalTargetToken++)
                      .first;
        binding.canonicalTargetToken = token->second;
        binding.targetHasWinner = getATFieldSymbolWinner(
            target, binding.winnerInputOccurrence,
            binding.winnerInputSymbolIndex, binding.winnerInputSectionIndex);
        binding.targetDefined = target->isDefined();
        if (binding.targetDefined) {
          auto *defined = llvm::cast<Defined>(target);
          binding.targetHasSection = defined->section != nullptr;
          binding.targetAbsolute = !binding.targetHasSection;
          binding.targetSynthetic =
              binding.targetHasSection &&
              llvm::isa<SyntheticSection>(defined->section);
          binding.targetOutputRva = target->getVA(ctx);
        }
      }
      bindings.push_back(binding);
    }
  }
  std::sort(bindings.begin(), bindings.end(),
            [](const ATFieldInputSymbolBinding &a,
               const ATFieldInputSymbolBinding &b) {
              if (a.inputOccurrence != b.inputOccurrence)
                return a.inputOccurrence < b.inputOccurrence;
              return a.inputSymbolIndex < b.inputSymbolIndex;
            });
  bindings.erase(
      std::unique(bindings.begin(), bindings.end(),
                  [](const ATFieldInputSymbolBinding &a,
                     const ATFieldInputSymbolBinding &b) {
                    return a.inputOccurrence == b.inputOccurrence &&
                           a.inputSymbolIndex == b.inputSymbolIndex;
                  }),
      bindings.end());
  current->onInputSymbolBindings(bindings);
}

void lld::elf::notifyATFieldPayloadIncluded(InputFile *file) noexcept {
  auto *current = getATFieldInputObserver();
  if (!current || file->lazy || file->atfieldIncluded ||
      (file->kind() != InputFile::ObjKind &&
       file->kind() != InputFile::BitcodeKind))
    return;
  file->atfieldIncluded = true;
  file->atfieldPayloadOrdinal = nextATFieldPayloadOrdinal();
  ATFieldPayloadIncludedEvent event;
  event.payloadOrdinal = file->atfieldPayloadOrdinal;
  event.inputOccurrence = file->atfieldInputOccurrence;
  event.archiveOccurrence = file->atfieldArchiveOccurrence;
  event.memberOccurrence = file->atfieldMemberOccurrence;
  event.argumentOrdinal = file->atfieldArgumentOrdinal;
  event.groupId = file->atfieldGroupId;
  event.childHeaderOffset = file->atfieldChildHeaderOffset;
  event.memberOrdinal = file->atfieldMemberOrdinal;
  event.thinArchive = file->atfieldExternalMemberBytes;
  event.reason = file->atfieldInclusionReason;
  event.trigger = file->atfieldTrigger;
  current->onPayloadIncluded(event);
  state().payloadIncludedEvents.push_back(event);
}

void lld::elf::notifyATFieldPayloadIncludedSnapshot() noexcept {
  auto *current = getATFieldInputObserver();
  if (!current)
    return;
  auto &s = state();
  if (s.payloadSnapshotNotified)
    return;
  s.payloadSnapshotNotified = true;
  current->onPayloadIncludedSnapshot(s.payloadIncludedEvents);
}

void lld::elf::notifyATFieldParse(InputFile *file) noexcept {
  auto *current = getATFieldInputObserver();
  if (!current || (file->kind() != InputFile::ObjKind &&
                   file->kind() != InputFile::BitcodeKind))
    return;
  ATFieldInputParseEvent event;
  event.inputOccurrence = file->atfieldInputOccurrence;
  event.archiveOccurrence = file->atfieldArchiveOccurrence;
  event.memberOccurrence = file->atfieldMemberOccurrence;
  event.argumentOrdinal = file->atfieldArgumentOrdinal;
  event.groupId = file->atfieldGroupId;
  event.kind = file->kind() == InputFile::BitcodeKind
                   ? ATFieldInputKind::Bitcode
                   : ATFieldInputKind::ETRel;
  event.externalMemberBytes = file->atfieldExternalMemberBytes;
  event.path = file->getName();
  event.contents = file->mb;
  current->onParse(event);
}

void lld::elf::notifyATFieldLinkerScript(
    uint64_t argumentOrdinal, llvm::StringRef path,
    llvm::MemoryBufferRef contents, bool nested,
    ATFieldOccurrence nestedOccurrence) noexcept {
  if (auto *current = getATFieldInputObserver()) {
    ATFieldArgumentContext context = getATFieldArgumentContext();
    ATFieldLinkerScriptEvent event;
    event.scriptOccurrence =
        nested ? (nestedOccurrence ? nestedOccurrence : nextATFieldOccurrence())
               : (context.scriptOccurrence == 0
                      ? nextATFieldOccurrence()
                      : context.scriptOccurrence);
    event.parentScriptOccurrence = nested ? context.scriptOccurrence : 0;
    event.argumentOrdinal = argumentOrdinal;
    event.kind = context.scriptKind;
    event.path = path;
    event.contents = contents;
    current->onLinkerScript(event);
    if (!nested)
      state().scriptOccurrences[argumentOrdinal] = event.scriptOccurrence;
    context.scriptOccurrence = event.scriptOccurrence;
    setATFieldArgumentContext(context);
  }
}
