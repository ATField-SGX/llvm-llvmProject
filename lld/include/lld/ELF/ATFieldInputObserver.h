//===- ATFieldInputObserver.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_AT_FIELD_INPUT_OBSERVER_H
#define LLD_ELF_AT_FIELD_INPUT_OBSERVER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <vector>

namespace lld::elf {

struct Ctx;
class InputFile;
class Symbol;
class ATFieldInputObserver;
class ATFieldPreparedInputProvider;
using ATFieldOccurrence = uint64_t;

struct ATFieldPreparedInputKey {
  uint64_t argumentOrdinal = 0;
  uint64_t archiveOccurrence = 0;
  uint64_t archiveChildHeaderOffset = 0;
  uint64_t memberOrdinal = 0;
  bool direct = false;
  bool thinArchive = false;
};
struct ATFieldPreparedInputReplacement {
  bool eligible = true;
  bool replaced = false;
  llvm::MemoryBufferRef contents;
};
struct ATFieldPreparedLinkerScriptReplacement {
  bool replaced = false;
  uint64_t scriptOccurrence = 0;
  llvm::MemoryBufferRef contents;
};
class ATFieldPreparedInputProvider {
public:
  virtual ~ATFieldPreparedInputProvider() = default;
  virtual llvm::Expected<uint64_t>
  translateArgumentOrdinal(uint64_t currentOrdinal) {
    return currentOrdinal;
  }
  virtual llvm::Expected<uint64_t>
  translateArchiveOccurrence(uint64_t currentOccurrence,
                             uint64_t argumentOrdinal,
                             uint64_t encounterOrdinal) {
    (void)argumentOrdinal;
    (void)encounterOrdinal;
    return currentOccurrence;
  }
  virtual llvm::Expected<ATFieldPreparedLinkerScriptReplacement>
  provideNestedLinkerScript(uint64_t argumentOrdinal,
                            uint64_t parentScriptOccurrence) {
    (void)argumentOrdinal;
    (void)parentScriptOccurrence;
    return ATFieldPreparedLinkerScriptReplacement{};
  }
  virtual llvm::Expected<uint64_t>
  translateLinkerScriptOccurrence(uint64_t currentOccurrence,
                                  uint64_t argumentOrdinal) {
    (void)argumentOrdinal;
    return currentOccurrence;
  }
  virtual llvm::Expected<uint64_t>
  translateArchiveMemberOccurrence(
      uint64_t currentOccurrence, uint64_t archiveOccurrence,
      uint64_t argumentOrdinal, uint64_t childHeaderOffset,
      uint64_t memberOrdinal, bool thinArchive) {
    (void)archiveOccurrence;
    (void)argumentOrdinal;
    (void)childHeaderOffset;
    (void)memberOrdinal;
    (void)thinArchive;
    return currentOccurrence;
  }
  virtual llvm::Expected<ATFieldPreparedInputReplacement>
  provide(const ATFieldPreparedInputKey &) = 0;
};

enum class ATFieldLinkArgumentKind : uint8_t {
  DirectInput, Archive, StartGroup, EndGroup, WholeOn, WholeOff, StartLib,
  EndLib, Script, Other,
};
enum class ATFieldLinkArgumentPolicy : uint8_t {
  PreparedInput, ReplaceScript, ReplaceOutput, DropSearch,
  RejectExternalInput, RejectArchiveSensitive, RetainPure,
};
enum class ATFieldInputKind : uint8_t { Unknown, ETRel, Bitcode, Other };
enum class ATFieldInputInclusionReason : uint8_t { Direct, WholeArchive, Lazy };
enum class ATFieldLinkerScriptKind : uint8_t { Linker = 0, Version = 1 };

struct ATFieldLinkArgumentEvent {
  uint64_t argumentOrdinal = 0;
  ATFieldLinkArgumentKind kind = ATFieldLinkArgumentKind::Other;
  ATFieldOccurrence inputOccurrence = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence scriptOccurrence = 0;
  uint32_t groupId = 0;
  bool wholeArchive = false;
  llvm::StringRef argument;
  llvm::StringRef path;
  llvm::StringRef diagnosticText;
  llvm::ArrayRef<const char *> tokens;
  ATFieldLinkArgumentPolicy policy = ATFieldLinkArgumentPolicy::RetainPure;
};
struct ATFieldDirectInputAdmissionEvent {
  uint64_t argumentOrdinal = 0;
  ATFieldOccurrence inputOccurrence = 0;
  uint32_t groupId = 0;
  llvm::StringRef path;
  llvm::MemoryBufferRef contents;
};
struct ATFieldArchiveEncounterEvent {
  uint64_t argumentOrdinal = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  uint32_t groupId = 0;
  bool wholeArchive = false;
  bool thinArchive = false;
  llvm::StringRef path;
  llvm::MemoryBufferRef contents;
  uint64_t memberCount = 0;
};
struct ATFieldArchiveMemberCandidateEvent {
  uint64_t argumentOrdinal = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence memberOccurrence = 0;
  ATFieldOccurrence inputOccurrence = 0;
  uint32_t groupId = 0;
  bool wholeArchive = false;
  uint64_t childHeaderOffset = 0;
  uint64_t memberOrdinal = 0;
  uint64_t memberSize = 0;
  ATFieldInputKind kind = ATFieldInputKind::Unknown;
  llvm::StringRef archivePath;
  llvm::StringRef memberName;
  llvm::MemoryBufferRef contents;
};
struct ATFieldPayloadIncludedEvent {
  uint64_t payloadOrdinal = 0;
  ATFieldOccurrence inputOccurrence = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence memberOccurrence = 0;
  uint64_t argumentOrdinal = 0;
  uint32_t groupId = 0;
  uint64_t childHeaderOffset = 0;
  uint64_t memberOrdinal = 0;
  bool thinArchive = false;
  ATFieldInputInclusionReason reason = ATFieldInputInclusionReason::Direct;
  llvm::StringRef trigger;
};
struct ATFieldInputParseEvent {
  ATFieldOccurrence inputOccurrence = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence memberOccurrence = 0;
  uint64_t argumentOrdinal = 0;
  uint32_t groupId = 0;
  ATFieldInputKind kind = ATFieldInputKind::ETRel;
  bool externalMemberBytes = false;
  llvm::StringRef path;
  llvm::MemoryBufferRef contents;
};
struct ATFieldLinkerScriptEvent {
  ATFieldOccurrence scriptOccurrence = 0;
  ATFieldOccurrence parentScriptOccurrence = 0;
  uint64_t argumentOrdinal = 0;
  ATFieldLinkerScriptKind kind = ATFieldLinkerScriptKind::Linker;
  llvm::StringRef path;
  llvm::MemoryBufferRef contents;
};
struct ATFieldInputSectionPiece {
  uint64_t inputOffset = 0;
  uint64_t size = 0;
  uint64_t outputOffset = ~uint64_t(0);
  bool live = false;
  bool cie = false;
  uint64_t firstRelocationIndex = ~uint64_t(0);
  uint64_t rawTargetInputSymbolIndex = ~uint64_t(0);
  ATFieldOccurrence resolvedTargetInputOccurrence = 0;
  uint64_t resolvedTargetInputSymbolIndex = ~uint64_t(0);
  uint32_t resolvedTargetInputSectionIndex = ~uint32_t(0);
  bool resolvedTargetHasWinner = false;
  bool targetDefined = false;
  bool targetFolded = false;
  bool targetHasSection = false;
  uint32_t targetPartition = 0;
};
struct ATFieldInputSectionResolutionEvent {
  ATFieldOccurrence inputOccurrence = 0;
  uint64_t argumentOrdinal = 0;
  uint32_t groupId = 0;
  uint32_t inputSectionIndex = 0;
  llvm::StringRef inputSectionName;
  uint32_t inputSectionType = 0;
  uint64_t inputSectionFlags = 0;
  uint64_t inputSectionSize = 0;
  bool live = false;
  bool hasOutputSection = false;
  uint32_t outputSectionIndex = 0;
  llvm::StringRef outputSectionName;
  uint32_t outputSectionType = 0;
  uint64_t outputSectionFlags = 0;
  uint64_t outputSectionVA = 0;
  uint64_t outputSectionFileOffset = 0;
  uint64_t outputSectionSize = 0;
  uint64_t outputSectionOffset = 0;
  bool present = true;
  bool discarded = false;
  llvm::ArrayRef<ATFieldInputSectionPiece> pieces;
};
struct ATFieldInputSectionSnapshot {
  bool present = false;
  bool discarded = false;
  llvm::StringRef name;
  uint32_t type = 0;
  uint64_t flags = 0;
  uint64_t size = 0;
};
struct ATFieldSymbolWinnerEvent {
  llvm::StringRef canonicalName;
  ATFieldOccurrence inputOccurrence = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence memberOccurrence = 0;
  uint32_t groupId = 0;
  uint64_t inputSymbolIndex = 0;
  uint32_t inputSectionIndex = 0;
  uint8_t inputBinding = 0;
  uint8_t inputType = 0;
  uint8_t inputVisibility = 0;
  uint32_t outputSectionIndex = 0;
  uint64_t outputRva = 0;
  bool common = false;
  bool weak = false;
  bool comdat = false;
};
struct ATFieldSymbolWinnerKey {
  ATFieldOccurrence inputOccurrence = 0;
  uint64_t inputSymbolIndex = 0;
};
struct ATFieldInputSymbolBinding {
  ATFieldOccurrence inputOccurrence = 0;
  uint64_t inputSymbolIndex = 0;
  uint64_t canonicalTargetToken = 0;
  bool targetHasWinner = false;
  ATFieldOccurrence winnerInputOccurrence = 0;
  uint64_t winnerInputSymbolIndex = ~uint64_t(0);
  uint32_t winnerInputSectionIndex = ~uint32_t(0);
  bool targetDefined = false;
  bool targetHasSection = false;
  bool targetAbsolute = false;
  bool targetSynthetic = false;
  uint64_t targetOutputRva = 0;
};
struct ATFieldArgumentContext {
  uint64_t argumentOrdinal = 0;
  uint32_t groupId = 0;
  bool wholeArchive = false;
  bool active = false;
  ATFieldLinkArgumentKind kind = ATFieldLinkArgumentKind::Other;
  ATFieldLinkArgumentPolicy policy =
      ATFieldLinkArgumentPolicy::RejectExternalInput;
  ATFieldLinkerScriptKind scriptKind = ATFieldLinkerScriptKind::Linker;
  ATFieldOccurrence inputOccurrence = 0;
  ATFieldOccurrence archiveOccurrence = 0;
  ATFieldOccurrence scriptOccurrence = 0;
  llvm::StringRef argument;
  llvm::StringRef path;
  llvm::StringRef diagnosticText;
};

struct ATFieldSymbolCandidate {
  InputFile *file = nullptr;
  uint64_t inputSymbolIndex = 0;
  uint32_t inputSectionIndex = 0;
  uint8_t inputBinding = 0;
  uint8_t inputType = 0;
  uint8_t inputVisibility = 0;
  bool common = false;
  bool weak = false;
  bool comdat = false;
};

struct ATFieldObserverState {
  ATFieldInputObserver *observer = nullptr;
  ATFieldPreparedInputProvider *preparedInputProvider = nullptr;
  ATFieldOccurrence nextOccurrence = 1;
  uint64_t payloadOrdinal = 0;
  ATFieldArgumentContext argumentContext;
  llvm::DenseMap<Symbol *, ATFieldSymbolCandidate> candidate;
  llvm::DenseMap<Symbol *, ATFieldSymbolCandidate> winners;
  llvm::DenseMap<Symbol *, uint64_t> canonicalTargetTokens;
  uint64_t nextCanonicalTargetToken = 1;
  llvm::DenseMap<uint64_t, ATFieldOccurrence> scriptOccurrences;
  llvm::DenseMap<uint64_t, uint64_t> archiveEncounterOrdinals;
  llvm::SmallVector<ATFieldPayloadIncludedEvent, 0> payloadIncludedEvents;
  bool terminalNotified = false;
};

// Event strings and buffers are borrowed until the synchronous callback returns.
class ATFieldInputObserver {
public:
  virtual ~ATFieldInputObserver() = default;
  virtual void onLinkArgument(const ATFieldLinkArgumentEvent &) {}
  virtual void onLinkArgumentsComplete(uint64_t) {}
  virtual void onDirectInputAdmission(
      const ATFieldDirectInputAdmissionEvent &) {}
  virtual void onArchiveEncounter(const ATFieldArchiveEncounterEvent &) {}
  virtual void onArchiveMemberCandidate(
      const ATFieldArchiveMemberCandidateEvent &) {}
  virtual void onPayloadIncluded(const ATFieldPayloadIncludedEvent &) {}
  virtual void onPayloadIncludedSnapshot(
      llvm::ArrayRef<ATFieldPayloadIncludedEvent>) {}
  virtual void onParse(const ATFieldInputParseEvent &) {}
  virtual void onLinkerScript(const ATFieldLinkerScriptEvent &) {}
  virtual void onInputSectionResolved(
      const ATFieldInputSectionResolutionEvent &) {}
  virtual void onSymbolWinner(const ATFieldSymbolWinnerEvent &) {}
  virtual void onExpectedSymbolWinnerKeys(
      llvm::ArrayRef<ATFieldSymbolWinnerKey>) {}
  virtual void onInputSymbolBindings(
      llvm::ArrayRef<ATFieldInputSymbolBinding>) {}
};
ATFieldInputObserver *setATFieldInputObserver(
    ATFieldInputObserver *) noexcept;
ATFieldInputObserver *getATFieldInputObserver() noexcept;
ATFieldPreparedInputProvider *setATFieldPreparedInputProvider(
    ATFieldPreparedInputProvider *) noexcept;
ATFieldPreparedInputProvider *getATFieldPreparedInputProvider() noexcept;
uint64_t translateATFieldArgumentOrdinal(uint64_t) noexcept;
uint64_t translateATFieldArchiveOccurrence(uint64_t,
                                           uint64_t argumentOrdinal,
                                           uint64_t encounterOrdinal) noexcept;
uint64_t translateATFieldArchiveMemberOccurrence(
    uint64_t, uint64_t archiveOccurrence, uint64_t argumentOrdinal,
    uint64_t childHeaderOffset, uint64_t memberOrdinal,
    bool thinArchive) noexcept;
uint64_t nextATFieldArchiveEncounterOrdinal(uint64_t argumentOrdinal) noexcept;
class ATFieldInputObserverScope {
public:
  explicit ATFieldInputObserverScope(ATFieldInputObserver *observer) noexcept
      : previous(setATFieldInputObserver(observer)) {}
  ~ATFieldInputObserverScope() { setATFieldInputObserver(previous); }
  ATFieldInputObserverScope(const ATFieldInputObserverScope &) = delete;
  ATFieldInputObserverScope &operator=(const ATFieldInputObserverScope &) =
      delete;

private:
  ATFieldInputObserver *previous;
};
class ATFieldPreparedInputProviderScope {
public:
  explicit ATFieldPreparedInputProviderScope(
      ATFieldPreparedInputProvider *provider) noexcept
      : previous(setATFieldPreparedInputProvider(provider)) {}
  ~ATFieldPreparedInputProviderScope() {
    setATFieldPreparedInputProvider(previous);
  }
  ATFieldPreparedInputProviderScope(
      const ATFieldPreparedInputProviderScope &) = delete;
  ATFieldPreparedInputProviderScope &operator=(
      const ATFieldPreparedInputProviderScope &) = delete;

private:
  ATFieldPreparedInputProvider *previous;
};
ATFieldOccurrence nextATFieldOccurrence() noexcept;
uint64_t nextATFieldPayloadOrdinal() noexcept;
void beginATFieldLink() noexcept;
ATFieldArgumentContext getATFieldArgumentContext() noexcept;
void setATFieldArgumentContext(const ATFieldArgumentContext &) noexcept;
void clearATFieldArgumentContext() noexcept;
void notifyATFieldParse(class InputFile *) noexcept;
void notifyATFieldPayloadIncluded(class InputFile *) noexcept;
void notifyATFieldLinkerScript(uint64_t, llvm::StringRef,
                               llvm::MemoryBufferRef, bool nested = false,
                               ATFieldOccurrence nestedOccurrence = 0) noexcept;
ATFieldOccurrence ensureATFieldScriptOccurrence(uint64_t) noexcept;
void notifyATFieldInputSections() noexcept;
bool claimATFieldTerminalNotification() noexcept;
void setATFieldSymbolCandidate(Symbol *, InputFile *, uint64_t, uint32_t,
                               uint8_t, uint8_t, uint8_t, bool, bool, bool);
void clearATFieldSymbolCandidate() noexcept;
void noteATFieldSymbolWinner(Symbol *) noexcept;
bool getATFieldSymbolWinner(Symbol *, ATFieldOccurrence &, uint64_t &,
                            uint32_t &) noexcept;
void notifyATFieldSymbolWinners(Ctx &, llvm::ArrayRef<Symbol *>) noexcept;

} // namespace lld::elf

#endif
