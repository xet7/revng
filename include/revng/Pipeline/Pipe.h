#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Error.h"

#include "revng/ADT/STLExtras.h"
#include "revng/Pipeline/Container.h"
#include "revng/Pipeline/ContainerSet.h"
#include "revng/Pipeline/Contract.h"
#include "revng/Pipeline/ExecutionContext.h"
#include "revng/Pipeline/Invokable.h"
#include "revng/Pipeline/Target.h"
#include "revng/Support/Debug.h"

namespace pipeline {

/// rappresents the requested (not expected, which means that it contains only
/// the targets the user care about, not all those that will be generated as a
/// side effect) input and output of a given invocation of a pipe.
class PipeExecutionEntry {
public:
  ContainerToTargetsMap Output;
  ContainerToTargetsMap Input;

  PipeExecutionEntry(ContainerToTargetsMap Output,
                     ContainerToTargetsMap Input) :
    Output(std::move(Output)), Input(std::move(Input)) {}
};

namespace detail {

template<typename T>
concept HasContract = requires(T P) {
  { llvm::ArrayRef<ContractGroup>(P.getContract()) };
};

template<typename T, typename FirstRunArg, typename... Args>
concept Pipe = Invokable<T, FirstRunArg, Args...>
               and (IsContainer<Args> and ...) and HasContract<T>;

template<typename C, typename First, typename... Rest>
constexpr bool checkPipe(auto (C::*)(First, Rest...))
  requires Pipe<C, First, Rest...>
{
  return true;
}

template<typename C, typename First, typename... Rest>
constexpr size_t countArgs(auto (C::*)(First, Rest...)) {
  return sizeof...(Rest);
}

template<typename First, typename... Types>
const char *getNameOfContainerImpl(size_t Index) {
  if (Index == 0)
    return std::decay_t<First>::Name;

  if constexpr (sizeof...(Types) == 0)
    return "";
  else
    return getNameOfContainerImpl<Types...>(Index - 1);
}

template<typename C, typename Context, typename... Rest>
const char *getNameOfContainer(auto (C::*)(Context, Rest...), size_t Index) {
  return getNameOfContainerImpl<Rest...>(Index);
}

template<typename PipeType>
class PipeWrapperImpl;

// TODO: Rename, there are 3 layers of wrappers around a pipe and they are
// getting confusing
class PipeWrapperBase : public InvokableWrapperBase {
public:
  template<typename PipeType>
  using ImplType = PipeWrapperImpl<PipeType>;

public:
  virtual PipeExecutionEntry
  getRequirements(const Context &Ctx,
                  const ContainerToTargetsMap &Target) const = 0;

  virtual ContainerToTargetsMap
  deduceResults(const Context &Ctx, ContainerToTargetsMap &Target) const = 0;

  virtual bool areRequirementsMet(const Context &Ctx,
                                  const ContainerToTargetsMap &Input) const = 0;

  virtual std::unique_ptr<PipeWrapperBase>
  clone(std::vector<std::string> NewRunningContainersNames = {}) const = 0;

  virtual llvm::Error checkPrecondition(const Context &Ctx) const = 0;

  virtual size_t getContainerArgumentsCount() const = 0;

  virtual llvm::StringRef getContainerName(size_t Index) const = 0;

  virtual ~PipeWrapperBase() = default;
};

/// A pipe must be type erased somehow to become compatible with a pipeline,
/// a PipeWrapperImpl takes care of this issue, it can be constructed from
/// any pipeline type, and it will expose the contract and run method of that
/// enforcer.
template<typename PipeType>
class PipeWrapperImpl : public PipeWrapperBase {
private:
  static constexpr bool CheckPipe = checkPipe(&PipeType::run);
  static_assert(CheckPipe);

  InvokableWrapperImpl<PipeType> Invokable;

public:
  PipeWrapperImpl(PipeType ActualPipe,
                  std::vector<std::string> RunningContainersNames) :
    Invokable(std::move(ActualPipe), std::move(RunningContainersNames)) {}

  PipeWrapperImpl(const PipeWrapperImpl &ActualPipe,
                  std::vector<std::string> RunningContainersNames) :
    Invokable(ActualPipe.Invokable, std::move(RunningContainersNames)) {}

  PipeWrapperImpl(PipeWrapperImpl &&ActualPipe,
                  std::vector<std::string> RunningContainersNames) :
    Invokable(std::move(ActualPipe.Invokable),
              std::move(RunningContainersNames)) {}

public:
  bool areRequirementsMet(const Context &Ctx,
                          const ContainerToTargetsMap &Input) const override {
    const auto &Contracts = Invokable.getPipe().getContract();
    if (Contracts.size() == 0)
      return true;

    ContainerToTargetsMap ToCheck = Input;
    for (const auto &Contract : Contracts) {
      if (Contract.forwardMatches(Ctx,
                                  ToCheck,
                                  Invokable.getRunningContainersNames()))
        return true;

      Contract.deduceResults(Ctx,
                             ToCheck,
                             Invokable.getRunningContainersNames());
    }

    return false;
  }

  PipeExecutionEntry
  getRequirements(const Context &Ctx,
                  const ContainerToTargetsMap &Target) const override {
    const auto &Contracts = Invokable.getPipe().getContract();
    auto Requirements = Target;
    for (const auto &Contract : llvm::reverse(Contracts))
      Requirements = Contract
                       .deduceRequirements(Ctx,
                                           Requirements,
                                           Invokable
                                             .getRunningContainersNames());
    auto TargetsProducedByMe = Target;
    TargetsProducedByMe.erase(Requirements);

    return PipeExecutionEntry(TargetsProducedByMe, Requirements);
  }

  ContainerToTargetsMap
  deduceResults(const Context &Ctx,
                ContainerToTargetsMap &Target) const override {
    const auto &Contracts = Invokable.getPipe().getContract();
    for (const auto &Contract : Contracts)
      Contract.deduceResults(Ctx,
                             Target,
                             Invokable.getRunningContainersNames());
    return Target;
  }

  std::unique_ptr<PipeWrapperBase>
  clone(std::vector<std::string> NewContainersNames = {}) const override {
    if (NewContainersNames.empty())
      return std::make_unique<PipeWrapperImpl>(*this);
    return std::make_unique<PipeWrapperImpl>(*this,
                                             std::move(NewContainersNames));
  }

  llvm::Error checkPrecondition(const Context &Ctx) const override {
    return Invokable.getPipe().checkPrecondition(Ctx);
  }

  size_t getContainerArgumentsCount() const override {
    return countArgs(&PipeType::run);
  }

  llvm::StringRef getContainerName(size_t Index) const override {
    return getNameOfContainer(&PipeType::run, Index);
  }

public:
  void dump(std::ostream &OS,
            size_t Indentation) const override debug_function {
    Invokable.dump(OS, Indentation);
  }

  llvm::Error run(ExecutionContext &Ctx,
                  ContainerSet &Containers,
                  const llvm::StringMap<std::string> &ExtraArgs) override {
    return Invokable.run(Ctx, Containers, ExtraArgs);
  }

  void invalidate(const GlobalTupleTreeDiff &Diff,
                  ContainerToTargetsMap &Map,
                  const ContainerSet &Containers) const override {
    return Invokable.invalidate(Diff, Map, Containers);
  }

  std::vector<std::string> getOptionsNames() const override {
    return Invokable.getOptionsNames();
  }

  std::vector<std::string> getOptionsTypes() const override {
    return Invokable.getOptionsTypes();
  }

  std::vector<std::string> getRunningContainersNames() const override {
    return Invokable.getRunningContainersNames();
  }
  bool isContainerArgumentConst(size_t ArgumentIndex) const override {
    return Invokable.isContainerArgumentConst(ArgumentIndex);
  }
  std::string getName() const override { return Invokable.getName(); }
};

} // namespace detail

// Due to invokable wrapper not being controllable by this file we need to have
// a extra wrapper that carries along the invalidation metadata too.
struct PipeWrapper {

  class InvalidationMetadata {
  private:
    llvm::StringMap<PathTargetBimap> PathCache;

  public:
    void registerTargetsDependingOn(const Context &Ctx,
                                    llvm::StringRef GlobalName,
                                    const TupleTreePath &Path,
                                    ContainerToTargetsMap &Out,
                                    Logger<> &Log) const {
      if (auto Iter = PathCache.find(GlobalName); Iter != PathCache.end()) {

        auto &Bimap = Iter->second;
        auto It = Bimap.find(Path);
        if (It == Bimap.end())
          return;

        if (Log.isEnabled()) {
          Log << "Registering: ";
          for (const auto &Entry : It->second) {
            Log << Entry.getTarget().serialize() << " in "
                << Entry.getContainerName() << "\n";
          }
          Log << DoLog;
        }

        for (const auto &Entry : It->second)
          Out.add(Entry.getContainerName(), Entry.getTarget());
      }
    }

    void remove(const ContainerToTargetsMap &Map) {
      for (auto &Pair : Map) {
        auto Iter = PathCache.find(Pair.first());
        if (Iter == PathCache.end())
          continue;

        Iter->second.remove(Pair.second, Pair.first());
      }
    }

    bool contains(llvm::StringRef GlobalName,
                  const TargetInContainer &Target) const {
      if (auto Iter = PathCache.find(GlobalName); Iter != PathCache.end())
        return Iter->second.contains(Target);
      return false;
    }

    const llvm::StringMap<PathTargetBimap> &getPathCache() const {
      return PathCache;
    }
    llvm::StringMap<PathTargetBimap> &getPathCache() { return PathCache; }

    const PathTargetBimap &getPathCache(llvm::StringRef GlobalName) const {
      revng_assert(PathCache.find(GlobalName) != PathCache.end());
      return PathCache.find(GlobalName)->second;
    }
    PathTargetBimap &getPathCache(llvm::StringRef GlobalName) {
      return PathCache[GlobalName];
    }
  };

public:
  using WrapperType = InvokableWrapper<detail::PipeWrapperBase>;
  WrapperType Pipe;
  InvalidationMetadata InvalidationMetadata;

  template<typename PipeType>
  static PipeWrapper
  make(PipeType Pipe, std::vector<std::string> RunningContainersNames) {
    return WrapperType::make<PipeType>(Pipe, std::move(RunningContainersNames));
  }

  template<typename PipeType>
  static PipeWrapper make(std::vector<std::string> RunningContainersNames) {
    return WrapperType::make<PipeType>(std::move(RunningContainersNames));
  }

  PipeWrapper(const InvokableWrapper<detail::PipeWrapperBase> &Other) :
    Pipe(Other) {}

  PipeWrapper(const PipeWrapper &Other,
              std::vector<std::string> RunningContainersNames) :
    Pipe(Other.Pipe, RunningContainersNames) {}

  template<typename PipeType, typename... ContainerNames>
  static PipeWrapper bind(ContainerNames &&...Names) {
    return WrapperType::bind<PipeType, ContainerNames...>(Names...);
  }

  template<typename PipeType, typename... ContainerNames>
  static PipeWrapper bind(PipeType &&E, ContainerNames &&...Names) {
    return WrapperType::bind<PipeType, ContainerNames...>(E, Names...);
  }
};

} // namespace pipeline
