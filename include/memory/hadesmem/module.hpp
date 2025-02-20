// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#pragma once

#include <cstring>
#include <functional>
#include <ostream>
#include <string>
#include <utility>

#include <windows.h>
#include <tlhelp32.h>

#include <hadesmem/config.hpp>
#include <hadesmem/detail/assert.hpp>
#include <hadesmem/detail/filesystem.hpp>
#include <hadesmem/detail/smart_handle.hpp>
#include <hadesmem/detail/toolhelp.hpp>
#include <hadesmem/detail/to_upper_ordinal.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/process.hpp>

namespace hadesmem
{
class Module
{
public:
  explicit Module(Process const& process, HMODULE handle) : process_{&process}
  {
    Initialize(handle);
  }

  explicit Module(Process const& process, std::wstring const& path)
    : process_{&process}
  {
    Initialize(path);
  }

  HMODULE GetHandle() const noexcept
  {
    return handle_;
  }

  DWORD GetSize() const noexcept
  {
    return size_;
  }

  std::wstring GetName() const
  {
    return name_;
  }

  std::wstring GetPath() const
  {
    return path_;
  }

private:
  template <typename ModuleT> friend class ModuleIterator;

  using EntryCallback = std::function<bool(MODULEENTRY32W const&)>;

  explicit Module(Process const& process, MODULEENTRY32W const& entry)
    : process_(&process), handle_(nullptr), size_(0), name_(), path_()
  {
    Initialize(entry);
  }

  void Initialize(HMODULE handle)
  {
    auto const handle_check = [&](MODULEENTRY32W const& entry) -> bool
    {
      return (entry.hModule == handle || !handle);
    };

    InitializeIf(handle_check);
  }

  void Initialize(std::wstring const& path)
  {
    bool const is_path = (path.find_first_of(L"\\/") != std::wstring::npos);

    std::wstring const path_upper = detail::ToUpperOrdinal(path);

    auto const path_check = [&](MODULEENTRY32W const& entry) -> bool
    {
      return is_path ? (detail::ArePathsEquivalent(path, entry.szExePath))
                     : (path_upper == detail::ToUpperOrdinal(entry.szModule));
    };

    InitializeIf(path_check);
  }

  void Initialize(MODULEENTRY32W const& entry)
  {
    handle_ = entry.hModule;
    size_ = entry.modBaseSize;
    name_ = entry.szModule;
    path_ = entry.szExePath;
  }

  void InitializeIf(EntryCallback const& check_func)
  {
    detail::SmartSnapHandle const snap{
      detail::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process_->GetId())};

    hadesmem::detail::Optional<MODULEENTRY32W> entry;
    for (entry = detail::Module32First(snap.GetHandle()); entry;
         entry = detail::Module32Next(snap.GetHandle()))
    {
      if (check_func(*entry))
      {
        Initialize(*entry);
        return;
      }
    }

    HADESMEM_DETAIL_THROW_EXCEPTION(Error{}
                                    << ErrorString{"Could not find module."});
  }

  Process const* process_;
  HMODULE handle_{nullptr};
  DWORD size_{0UL};
  std::wstring name_;
  std::wstring path_;
};

inline bool operator==(Module const& lhs,
                       Module const& rhs) noexcept
{
  return lhs.GetHandle() == rhs.GetHandle();
}

inline bool operator!=(Module const& lhs,
                       Module const& rhs) noexcept
{
  return !(lhs == rhs);
}

inline bool operator<(Module const& lhs,
                      Module const& rhs) noexcept
{
  return lhs.GetHandle() < rhs.GetHandle();
}

inline bool operator<=(Module const& lhs,
                       Module const& rhs) noexcept
{
  return lhs.GetHandle() <= rhs.GetHandle();
}

inline bool operator>(Module const& lhs,
                      Module const& rhs) noexcept
{
  return lhs.GetHandle() > rhs.GetHandle();
}

inline bool operator>=(Module const& lhs,
                       Module const& rhs) noexcept
{
  return lhs.GetHandle() >= rhs.GetHandle();
}

inline std::ostream& operator<<(std::ostream& lhs, Module const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << static_cast<void*>(rhs.GetHandle());
  lhs.imbue(old);
  return lhs;
}

inline std::wostream& operator<<(std::wostream& lhs, Module const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << static_cast<void*>(rhs.GetHandle());
  lhs.imbue(old);
  return lhs;
}
}
