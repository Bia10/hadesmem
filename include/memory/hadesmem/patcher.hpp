// Copyright (C) 2010-2013 Joshua Boyce.
// See the file COPYING for copying permission.

#pragma once

#include <climits>
#include <cstdint>
#include <locale>
#include <memory>
#include <sstream>
#include <type_traits>
#include <vector>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <asmjit/asmjit.h>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <udis86.h>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <windows.h>

#include <hadesmem/alloc.hpp>
#include <hadesmem/detail/assert.hpp>
#include <hadesmem/detail/union_cast.hpp>
#include <hadesmem/detail/trace.hpp>
#include <hadesmem/detail/type_traits.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/flush.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>
#include <hadesmem/thread_helpers.hpp>
#include <hadesmem/write.hpp>

// TODO: Fix exception safety.

// TODO: EAT hooking.

// TODO: IAT hooking.

// TODO: VEH hooking. (INT 3, DR, invalid instr, etc.)

// TODO: VMT hooking.

// TODO: Make hooking a transactional operation.

// TODO: Support 'safe' unloading by incrementing/decrementing a counter for
// each detour so it can be detect when your code is currently executing
// before unloading? What other options are there?

// TODO: Support passing a hook context. (This is needed to support multi-module
// support properly in base hook. i.e. Two concurrent D3D instances.) Need to be
// sure not to dirty registers though. Perhaps use a second trampoline in
// patcher when jumping to detour to pass a hook context (containing original
// trampoline, original module, etc).

// TODO: Rewrite to not use AsmJit.

// TODO: Add proper tests for edge cases trying to be handled (thread
// suspension, thread redirection, instruction resolution, no free trampoline
// blocks near a target address, short and far jumps, etc etc.)

// TODO: Review, refactor, rewrite, etc this entire module. Put TODOs where
// appropriate, remove and add APIs, fix bugs, clean up code, etc. Use new
// language features like noexcept, constexpr, etc. Consider other designs
// entirely.

// TODO: Add proper support for hooking different calling conventions without
// relying on the detour calling convention matching the target. Especially
// important for __thiscall etc where we're currently relying on undefined
// behaviour to convert a member fn pointer to a void*.

// TODO: Consolidate memory allocations where possible. Taking a page for
// every trampoline (including two trampolines per patch on x64 -- fix
// this too) is extremely wasteful. Perhaps allocate a block the size of
// the allocation granularity then use a custom heap?

namespace hadesmem
{

namespace detail
{

inline void VerifyPatchThreads(DWORD pid, void* target, std::size_t len)
{
  ThreadList threads(pid);
  for (auto const& thread_entry : threads)
  {
    if (thread_entry.GetId() == ::GetCurrentThreadId())
    {
      continue;
    }

    Thread const thread(thread_entry.GetId());
    auto const context = GetThreadContext(thread, CONTEXT_CONTROL);
#if defined(HADESMEM_DETAIL_ARCH_X64)
    auto const ip = reinterpret_cast<void const*>(context.Rip);
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    auto const ip = reinterpret_cast<void const*>(context.Eip);
#else
#error "[HadesMem] Unsupported architecture."
#endif
    if (target <= ip && ip < static_cast<std::uint8_t*>(target) + len)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error() << ErrorString("Thread is currently executing patch target."));
    }
  }
}
}

class PatchRaw
{
public:
  explicit PatchRaw(Process const& process,
                    PVOID target,
                    std::vector<BYTE> const& data)
    : process_(&process), applied_(false), target_(target), data_(data), orig_()
  {
  }

  PatchRaw(PatchRaw const& other) = delete;

  PatchRaw& operator=(PatchRaw const& other) = delete;

  PatchRaw(PatchRaw&& other)
    : process_(other.process_),
      applied_(other.applied_),
      target_(other.target_),
      data_(std::move(other.data_)),
      orig_(std::move(other.orig_))
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
  }

  PatchRaw& operator=(PatchRaw&& other)
  {
    RemoveUnchecked();

    process_ = other.process_;
    other.process_ = nullptr;

    applied_ = other.applied_;
    other.applied_ = false;

    target_ = other.target_;
    other.target_ = nullptr;

    data_ = std::move(other.data_);

    orig_ = std::move(other.orig_);

    return *this;
  }

  ~PatchRaw()
  {
    RemoveUnchecked();
  }

  bool IsApplied() const HADESMEM_DETAIL_NOEXCEPT
  {
    return applied_;
  }

  void Apply()
  {
    if (applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process(process_->GetId());

    detail::VerifyPatchThreads(process_->GetId(), target_, data_.size());

    orig_ = ReadVector<BYTE>(*process_, target_, data_.size());

    WriteVector(*process_, target_, data_);

    FlushInstructionCache(*process_, target_, data_.size());

    applied_ = true;
  }

  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process(process_->GetId());

    detail::VerifyPatchThreads(process_->GetId(), target_, data_.size());

    WriteVector(*process_, target_, orig_);

    FlushInstructionCache(*process_, target_, orig_.size());

    applied_ = false;
  }

private:
  void RemoveUnchecked() HADESMEM_DETAIL_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (...)
    {
      // WARNING: Patch may not be removed if Remove fails.
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
      HADESMEM_DETAIL_ASSERT(false);

      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      data_.clear();
      orig_.clear();
    }
  }

  Process const* process_;
  bool applied_;
  PVOID target_;
  std::vector<BYTE> data_;
  std::vector<BYTE> orig_;
};

class PatchDetour
{
public:
  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchDetour(Process const& process,
                       TargetFuncT target,
                       DetourFuncT detour)
    : process_(&process),
      applied_(false),
      target_(detail::UnionCast<PVOID>(target)),
      detour_(detail::UnionCast<PVOID>(detour)),
      trampoline_(),
      orig_(),
      trampolines_()
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<TargetFuncT>::value ||
                                  std::is_pointer<TargetFuncT>::value);
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<DetourFuncT>::value ||
                                  std::is_pointer<DetourFuncT>::value);
  }

  PatchDetour(PatchDetour const& other) = delete;

  PatchDetour& operator=(PatchDetour const& other) = delete;

  PatchDetour(PatchDetour&& other)
    : process_(other.process_),
      applied_(other.applied_),
      target_(other.target_),
      detour_(other.detour_),
      trampoline_(std::move(other.trampoline_)),
      orig_(std::move(other.orig_)),
      trampolines_(std::move(other.trampolines_))
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
    other.detour_ = nullptr;
  }

  PatchDetour& operator=(PatchDetour&& other)
  {
    RemoveUnchecked();

    process_ = other.process_;
    other.process_ = nullptr;

    applied_ = other.applied_;
    other.applied_ = false;

    target_ = other.target_;
    other.target_ = nullptr;

    detour_ = other.detour_;
    other.detour_ = nullptr;

    trampoline_ = std::move(other.trampoline_);

    orig_ = std::move(other.orig_);

    trampolines_ = std::move(other.trampolines_);

    return *this;
  }

  ~PatchDetour()
  {
    RemoveUnchecked();
  }

  void Apply()
  {
    if (applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process(process_->GetId());

    std::uint32_t const kMaxInstructionLen = 15;
    std::uint32_t const kTrampSize = kMaxInstructionLen * 3;

    trampoline_ = std::make_unique<Allocator>(*process_, kTrampSize);
    PBYTE tramp_cur = static_cast<PBYTE>(trampoline_->GetBase());

    std::vector<BYTE> const buffer(
      ReadVector<BYTE>(*process_, target_, kTrampSize));

    ud_t ud_obj;
    ud_init(&ud_obj);
    ud_set_input_buffer(&ud_obj, buffer.data(), buffer.size());
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_pc(&ud_obj, reinterpret_cast<std::uint64_t>(target_));
#if defined(_M_AMD64)
    ud_set_mode(&ud_obj, 64);
#elif defined(_M_IX86)
    ud_set_mode(&ud_obj, 32);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    bool detour_near = IsNear(target_, detour_);
    HADESMEM_DETAIL_TRACE_A(detour_near ? "Detour near." : "Detour far.");
    // TODO: Support push/ret WriteJump fallback for cases where we can't find a
    // trampoline.
    std::size_t const jump_size = detour_near ? kJumpSize32 : kJumpSize64;

    // TODO: Detect cases where hooking may overflow past the end of
    // a function, and fail. (Provide policy or flag to allow
    // overriding this behaviour.) Examples may be instructions such
    // as int 3, ret, jmp, etc.
    std::uint32_t instr_size = 0;
    do
    {
      std::uint32_t const len = ud_disassemble(&ud_obj);
      if (len == 0)
      {
        HADESMEM_DETAIL_THROW_EXCEPTION(Error()
                                        << ErrorString("Disassembly failed."));
      }

#if !defined(HADESMEM_NO_TRACE)
      char const* const asm_str = ud_insn_asm(&ud_obj);
      char const* const asm_bytes_str = ud_insn_hex(&ud_obj);
      HADESMEM_DETAIL_TRACE_FORMAT_A(
        "%s. [%s].",
        (asm_str ? asm_str : "Invalid."),
        (asm_bytes_str ? asm_bytes_str : "Invalid."));
#endif

      // TODO: Improve relative instruction rebuilding. x64 has far more IP
      // relative instructions than x86. Prioritize most common instructions
      // first, e.g. conditional jumps. This includes support for more operand
      // sizes for existing relative instruction support.
      // TODO: Improve instruction rebuilding for cases such as jumps backwards
      // into the detour and fail safely (or whatever is appropriate).
      ud_operand_t const* const op = ud_insn_opr(&ud_obj, 0);
      bool is_jimm = op && op->type == UD_OP_JIMM;
      // Handle JMP QWORD PTR [RIP+Rel32]. Necessary for hook chain support.
      // TODO: Support more types of memory operand jumps.
      bool is_jmem = op && op->type == UD_OP_MEM && op->base == UD_R_RIP &&
                     op->index == UD_NONE && op->scale == 0 && op->size == 0x40;
      if ((ud_obj.mnemonic == UD_Ijmp || ud_obj.mnemonic == UD_Icall) && op &&
          (is_jimm || is_jmem))
      {
        std::int64_t insn_target = 0;
        std::uint16_t const size = is_jimm ? op->size : op->offset;
        HADESMEM_DETAIL_TRACE_FORMAT_A("Operand/offset size is %hu.", size);
        switch (size)
        {
        case sizeof(std::int8_t) * CHAR_BIT:
          insn_target = op->lval.sbyte;
          break;
        case sizeof(std::int16_t) * CHAR_BIT:
          insn_target = op->lval.sword;
          break;
        case sizeof(std::int32_t) * CHAR_BIT:
          insn_target = op->lval.sdword;
          break;
        case sizeof(std::int64_t) * CHAR_BIT:
          insn_target = op->lval.sqword;
          break;
        default:
          HADESMEM_DETAIL_ASSERT(false);
          HADESMEM_DETAIL_THROW_EXCEPTION(
            Error() << ErrorString("Unknown instruction size."));
        }

        auto const resolve_rel = [](
          std::uint64_t base, std::int64_t target, std::uint32_t insn_len)
        {
          return reinterpret_cast<std::uint8_t*>(
                   static_cast<std::uintptr_t>(base)) +
                 target + insn_len;
        };

        std::uint64_t const insn_base = ud_insn_off(&ud_obj);
        std::uint32_t const insn_len = ud_insn_len(&ud_obj);

        auto const resolved_target =
          resolve_rel(insn_base, insn_target, insn_len);
        void* const jump_target =
          is_jimm ? resolved_target : Read<void*>(*process_, resolved_target);
        HADESMEM_DETAIL_TRACE_FORMAT_A("Jump target is 0x%p.", jump_target);
        if (ud_obj.mnemonic == UD_Ijmp)
        {
          tramp_cur += WriteJump(tramp_cur, jump_target, true);
        }
        else
        {
          HADESMEM_DETAIL_ASSERT(ud_obj.mnemonic == UD_Icall);
          tramp_cur += WriteCall(tramp_cur, jump_target);
        }
      }
      else
      {
        // TODO: Assert here on all known relative instructions on which we will
        // crash at runtime when executing the trampoline.
        uint8_t const* const raw = ud_insn_ptr(&ud_obj);
        Write(*process_, tramp_cur, raw, raw + len);
        tramp_cur += len;
      }

      instr_size += len;
    } while (instr_size < jump_size);

    tramp_cur +=
      WriteJump(tramp_cur, static_cast<PBYTE>(target_) + instr_size, true);

    FlushInstructionCache(
      *process_, trampoline_->GetBase(), trampoline_->GetSize());

    orig_ = ReadVector<BYTE>(*process_, target_, jump_size);

    // TODO: Instead of simply bailing in the case that this fails, we should
    // instead redirect the IP to the equivalent spot in our trampoline.
    detail::VerifyPatchThreads(process_->GetId(), target_, orig_.size());

    WriteJump(target_, detour_);

    FlushInstructionCache(*process_, target_, instr_size);

    applied_ = true;
  }

  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process(process_->GetId());

    // TODO: Verify whether we need to even check this...
    detail::VerifyPatchThreads(process_->GetId(), target_, orig_.size());
    // TODO: Instead of simply bailing in the case that this fails, we should
    // instead redirect the IP to the equivalent spot in the target.
    detail::VerifyPatchThreads(
      process_->GetId(), trampoline_->GetBase(), trampoline_->GetSize());

    WriteVector(*process_, target_, orig_);

    trampoline_.reset();

    trampolines_.clear();

    applied_ = false;
  }

  PVOID GetTrampoline() const HADESMEM_DETAIL_NOEXCEPT
  {
    return trampoline_->GetBase();
  }

  template <typename FuncT> FuncT GetTrampoline() const HADESMEM_DETAIL_NOEXCEPT
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<FuncT>::value ||
                                  std::is_pointer<FuncT>::value);
    return hadesmem::detail::UnionCastUnchecked<FuncT>(trampoline_->GetBase());
  }

private:
  void RemoveUnchecked() HADESMEM_DETAIL_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (...)
    {
      // WARNING: Patch may not be removed if Remove fails.
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
      HADESMEM_DETAIL_ASSERT(false);

      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      detour_ = nullptr;
      trampoline_.reset();
      orig_.clear();
      trampolines_.clear();
    }
  }

  // Inspired by EasyHook.
  std::unique_ptr<Allocator> AllocatePageNear(PVOID address)
  {
    SYSTEM_INFO sys_info;
    ZeroMemory(&sys_info, sizeof(sys_info));
    GetSystemInfo(&sys_info);
    DWORD const page_size = sys_info.dwPageSize;

#if defined(_M_AMD64)
    LONG_PTR const search_beg = (std::max)(
      reinterpret_cast<LONG_PTR>(address) - 0x7FFFFF00LL,
      reinterpret_cast<LONG_PTR>(sys_info.lpMinimumApplicationAddress));
    LONG_PTR const search_end = (std::min)(
      reinterpret_cast<LONG_PTR>(address) + 0x7FFFFF00LL,
      reinterpret_cast<LONG_PTR>(sys_info.lpMaximumApplicationAddress));

    std::unique_ptr<Allocator> trampoline;

    auto const allocate_tramp = [](Process const & process,
                                   PVOID addr,
                                   SIZE_T size)->std::unique_ptr<Allocator>
    {
      try
      {
        return std::make_unique<Allocator>(process, size, addr);
      }
      catch (std::exception const& /*e*/)
      {
        return std::unique_ptr<Allocator>();
      }
    };

    for (LONG_PTR base = reinterpret_cast<LONG_PTR>(address), index = 0;
         base + index < search_end || base - index > search_beg;
         index += page_size)
    {
      LONG_PTR const higher = base + index;
      if (higher < search_end)
      {
        if (trampoline = allocate_tramp(
              *process_, reinterpret_cast<PVOID>(higher), page_size))
        {
          break;
        }
      }

      LONG_PTR const lower = base - index;
      if (lower > search_beg)
      {
        if (trampoline = allocate_tramp(
              *process_, reinterpret_cast<PVOID>(lower), page_size))
        {
          break;
        }
      }
    }

    if (!trampoline)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error() << ErrorString("Failed to find trampoline memory block."));
    }

    return trampoline;
#elif defined(_M_IX86)
    (void)address;
    return std::make_unique<Allocator>(*process_, page_size);
#else
#error "[HadesMem] Unsupported architecture."
#endif
  }

  bool IsNear(void* address, void* target)
  {
#if defined(_M_AMD64)
    auto const rel = reinterpret_cast<std::intptr_t>(target) -
                     reinterpret_cast<std::intptr_t>(address) - 5;
    return rel > (std::numeric_limits<std::uint32_t>::min)() &&
           rel < (std::numeric_limits<std::uint32_t>::max)();
#elif defined(_M_IX86)
    (void)address;
    (void)target;
    return true;
#else
#error "[HadesMem] Unsupported architecture."
#endif
  }

  // TODO: Remove code duplication from WriteCall.
  std::size_t
    WriteJump(void* address, void* target, bool push_ret_fallback = false)
  {
    asmjit::JitRuntime runtime;
    std::size_t expected_stub_size = 0;
    std::vector<std::uint8_t> jump_buf;
    bool asmjit_trampoline = false;

#if defined(_M_AMD64)
    asmjit::x64::Assembler jit(&runtime);
    if (IsNear(address, target))
    {
      HADESMEM_DETAIL_TRACE_A("Using relative jump.");

      // JMP <Target, Relative>
      jit.jmp(target);

      // AsmJit allocates space for a trampoline in case the jump destination
      // is out of range. We've already accounted for that though, so after
      // code generation we just want to ensure the trampoline is unused
      // (otherwise we -- or asmjit -- have a bug) and then drop it.
      asmjit_trampoline = true;

      // JMP <Target, Relative>
      // JMP QWORD PTR [Trampoline]
      // DB 8 ; Trampoline
      expected_stub_size = 0x13;
    }
    else
    {
      std::unique_ptr<Allocator> trampoline;
      try
      {
        trampoline = AllocatePageNear(address);
      }
      catch (std::exception const& /*e*/)
      {
        // Don't need to do anything, we'll fall back to PUSH/RET.
      }

      if (trampoline)
      {
        HADESMEM_DETAIL_TRACE_A("Using trampoline jump.");

        void* tramp_addr = trampoline->GetBase();

        Write(*process_, tramp_addr, target);

        trampolines_.emplace_back(std::move(trampoline));

        // JMP QWORD PTR <Trampoline, Relative>
        asmjit::Label label(jit.newLabel());
        jit.bind(label);
        std::intptr_t const disp = static_cast<std::uint8_t*>(tramp_addr) -
                                   static_cast<std::uint8_t*>(address) -
                                   sizeof(std::int32_t);
        jit.jmp(asmjit::x64::qword_ptr(label, static_cast<std::int32_t>(disp)));

        expected_stub_size = kJumpSize64;
      }
      else
      {
        if (!push_ret_fallback)
        {
          // We're out of options...
          HADESMEM_DETAIL_THROW_EXCEPTION(
            Error() << ErrorString("Unable to use a relative or trampoline "
                                   "jump, and push/ret fallback is disabled."));
        }

        HADESMEM_DETAIL_TRACE_A("Using push/ret 'jump'.");

        auto const target_uint = reinterpret_cast<std::uintptr_t>(target);
        auto const target_high =
          static_cast<std::uint32_t>((target_uint >> 32) & 0xFFFFFFFF);
        auto const target_low =
          static_cast<std::uint32_t>(target_uint & 0xFFFFFFFF);

        if (target_high)
        {
          HADESMEM_DETAIL_TRACE_A("Push/ret 'jump' is big.");

          // PUSH DWORD <Target Low> ; Actually allocates 64-bytes of stack
          // space
          // MOV DWORD PTR [RSP+4], <Target High>
          // RET
          jit.push(target_low);
          // We could safe a few bytes here where this is some easy-to-generate
          // pattern (e.g. 0xFFFFFFFF), but we'll worry about that if we ever
          // actually need it.
          jit.mov(asmjit::x64::dword_ptr(asmjit::x64::rsp, 4), target_high);
          jit.ret();

          expected_stub_size = kPushRetSizeBig64;
        }
        else
        {
          HADESMEM_DETAIL_TRACE_A("Push/ret 'jump' is small.");

          // PUSH DWORD <Target Low> ; Actually allocates 64-bytes of stack
          // space
          // RET
          jit.push(target_low);
          jit.ret();

          expected_stub_size = kPushRetSizeSmall64;
        }
      }
    }

#elif defined(_M_IX86)
    (void)push_ret_fallback;
    asmjit::x86::Assembler jit(&runtime);
    // JMP <Target, Relative>
    jit.jmp(target);

    expected_stub_size = kJumpSize32;
#else
#error "[HadesMem] Unsupported architecture."
#endif

    std::size_t const stub_size = jit.getCodeSize();
    HADESMEM_DETAIL_TRACE_FORMAT_A(
      "Stub size = 0n%Iu, Expected stub size = 0n%Iu.",
      stub_size,
      expected_stub_size);
    HADESMEM_DETAIL_ASSERT(stub_size == expected_stub_size);
    if (stub_size != expected_stub_size)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(Error()
                                      << ErrorString("Unexpected stub size."));
    }

    jump_buf.resize(stub_size);

    std::size_t const stub_size_real =
      jit.relocCode(jump_buf.data(), reinterpret_cast<std::uintptr_t>(address));

    if (asmjit_trampoline == true)
    {
      HADESMEM_DETAIL_TRACE_FORMAT_A("Real stub size = 0n%Iu.", stub_size_real);
      HADESMEM_DETAIL_ASSERT(stub_size_real == kJumpSize32);
      if (stub_size_real != kJumpSize32)
      {
        HADESMEM_DETAIL_THROW_EXCEPTION(
          Error() << ErrorString("Unexpected real stub size."));
      }
      jump_buf.erase(std::begin(jump_buf) + stub_size_real, std::end(jump_buf));
      HADESMEM_DETAIL_ASSERT(jump_buf.size() == kJumpSize32);
    }

    WriteVector(*process_, address, jump_buf);

    return jump_buf.size();
  }

  std::size_t WriteCall(void* address, void* target)
  {
    asmjit::JitRuntime runtime;
    std::size_t expected_stub_size = 0;

#if defined(_M_AMD64)
    asmjit::x64::Assembler jit(&runtime);
    // TODO: Optimize this to avoid a trampoline where possible.
    std::unique_ptr<Allocator> trampoline = AllocatePageNear(address);

    PVOID tramp_addr = trampoline->GetBase();

    Write(*process_, tramp_addr, reinterpret_cast<std::uintptr_t>(target));

    trampolines_.emplace_back(std::move(trampoline));

    // CALL QWORD PTR <Trampoline, Relative>
    asmjit::Label label(jit.newLabel());
    jit.bind(label);
    std::intptr_t const disp = reinterpret_cast<std::intptr_t>(tramp_addr) -
                               reinterpret_cast<std::intptr_t>(address) -
                               sizeof(std::int32_t);
    jit.call(asmjit::x64::qword_ptr(label, static_cast<std::int32_t>(disp)));

    expected_stub_size = kCallSize64;
#elif defined(_M_IX86)
    asmjit::x86::Assembler jit(&runtime);
    // CALL <Target, Relative>
    jit.call(target);

    expected_stub_size = kCallSize32;
#else
#error "[HadesMem] Unsupported architecture."
#endif

    std::size_t const stub_size = jit.getCodeSize();
    HADESMEM_DETAIL_TRACE_FORMAT_A(
      "Stub size = 0n%Iu, Expected stub size = 0n%Iu.",
      static_cast<std::size_t>(stub_size),
      static_cast<std::size_t>(expected_stub_size));
    HADESMEM_DETAIL_ASSERT(stub_size == expected_stub_size);
    if (stub_size != expected_stub_size)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(Error()
                                      << ErrorString("Unexpected stub size."));
    }

    std::vector<std::uint8_t> jump_buf(stub_size);

    jit.relocCode(jump_buf.data(), reinterpret_cast<std::uintptr_t>(address));

    WriteVector(*process_, address, jump_buf);

    return stub_size;
  }

  static std::size_t const kJumpSize32 = 5;
  static std::size_t const kCallSize32 = 5;
#if defined(_M_AMD64)
  static std::size_t const kJumpSize64 = 6;
  static std::size_t const kCallSize64 = 6;
  static std::size_t const kPushRetSizeBig64 = 14;
  static std::size_t const kPushRetSizeSmall64 = 6;
#elif defined(_M_IX86)
  static std::size_t const kJumpSize64 = kJumpSize32;
  static std::size_t const kCallSize64 = kCallSize32;
#else
#error "[HadesMem] Unsupported architecture."
#endif

  Process const* process_;
  bool applied_;
  PVOID target_;
  PVOID detour_;
  std::unique_ptr<Allocator> trampoline_;
  std::vector<BYTE> orig_;
  std::vector<std::unique_ptr<Allocator>> trampolines_;
};
}
