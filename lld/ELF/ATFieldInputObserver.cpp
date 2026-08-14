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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>

using namespace lld::elf;

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
  s.scriptOccurrences.clear();
  s.archiveEncounterOrdinals.clear();
  s.payloadSnapshotNotified = false;
  s.terminalNotified = false;
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
bool lld::elf::claimATFieldTerminalNotification() noexcept {
  auto &s = state();
  if (s.terminalNotified)
    return false;
  s.terminalNotified = true;
  return true;
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

void lld::elf::setATFieldSymbolCandidate(
    Symbol *symbol, InputFile *file, uint64_t inputSymbolIndex,
    uint32_t inputSectionIndex, uint8_t inputBinding, uint8_t inputType,
    uint8_t inputVisibility, bool common, bool weak, bool comdat) {
  if (!getATFieldInputObserver() || !symbol || !file)
    return;
  state().candidate[symbol] = {file, inputSymbolIndex, inputSectionIndex,
                               inputBinding, inputType, inputVisibility,
                               common, weak, comdat};
}
void lld::elf::clearATFieldSymbolCandidate() noexcept {
  state().candidate.clear();
}
void lld::elf::noteATFieldSymbolWinner(Symbol *symbol) noexcept {
  auto &s = state();
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
void lld::elf::notifyATFieldSymbolWinners(
    Ctx &ctx, llvm::ArrayRef<Symbol *> symbols) noexcept {
  auto *current = getATFieldInputObserver();
  if (!current)
    return;
  auto &s = state();
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
