// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <windows.h>
#include <winnt.h>

#include <hadesmem/config.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/pelib/pe_file.hpp>
#include <hadesmem/pelib/nt_headers.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>
#include <hadesmem/write.hpp>

// TODO: In the case that PointerToRawData lies outside the file, the Windows PE
// loader considers both it and SizeOfRawData to be zero (which will result in
// the section being zero-filled). The exception to this is the last section,
// which apparently can't be incorrect like this. Find all cases which may be
// affected by this and handle them (e.g. imports, exports, resources, etc.).

namespace hadesmem
{
class Section
{
public:
  explicit Section(Process const& process, PeFile const& pe_file, void* base)
    : process_{&process},
      pe_file_{&pe_file},
      base_{static_cast<std::uint8_t*>(base)}
  {
    if (base_ == nullptr)
    {
      NtHeaders const nt_headers(process, pe_file);
      if (!nt_headers.GetNumberOfSections())
      {
        HADESMEM_DETAIL_THROW_EXCEPTION(
          Error{} << ErrorString{"Image nas no sections."});
      }

      if (pe_file_->Is64())
      {
        base_ = static_cast<PBYTE>(nt_headers.GetBase()) +
                offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                nt_headers.GetSizeOfOptionalHeader();
      }
      else
      {
        base_ = static_cast<PBYTE>(nt_headers.GetBase()) +
                offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                nt_headers.GetSizeOfOptionalHeader();
      }
    }

    // TODO: Does GetBase need to be adjusted for virtual sections?
    void const* const file_end =
      static_cast<std::uint8_t*>(pe_file.GetBase()) + pe_file.GetSize();
    void const* const section_hdr_next =
      reinterpret_cast<PIMAGE_SECTION_HEADER>(base_) + 1;
    if (pe_file.GetType() == PeFileType::Data && section_hdr_next > file_end)
    {
      // TODO: Support partial overlap by actually reading as much as we can,
      // rather than just setting everything to zero.
      is_virtual_ = true;
      ::ZeroMemory(&data_, sizeof(data_));
    }
    else
    {
      UpdateRead();
    }
  }

  explicit Section(Process const&& process, PeFile const& pe_file, void* base);

  explicit Section(Process const& process, PeFile&& pe_file, void* base);

  explicit Section(Process const&& process, PeFile&& pe_file, void* base);

  void* GetBase() const noexcept
  {
    return base_;
  }

  // TODO: Fix this hack.
  bool IsVirtual() const noexcept
  {
    return is_virtual_;
  }

  // TODO: Support virtual sections (incl. partial) properly. Currently we're
  // reading garbage.
  void UpdateRead()
  {
    data_ = Read<IMAGE_SECTION_HEADER>(*process_, base_);
  }

  void UpdateWrite()
  {
    Write(*process_, base_, data_);
  }

  std::string GetName() const
  {
    std::string name;
    for (std::size_t i = 0; i < 8 && data_.Name[i]; ++i)
    {
      name += data_.Name[i];
    }

    return name;
  }

  DWORD GetVirtualAddress() const
  {
    return data_.VirtualAddress;
  }

  DWORD GetVirtualSize() const
  {
    return data_.Misc.VirtualSize;
  }

  DWORD GetSizeOfRawData() const
  {
    return data_.SizeOfRawData;
  }

  DWORD GetPointerToRawData() const
  {
    return data_.PointerToRawData;
  }

  DWORD GetPointerToRelocations() const
  {
    return data_.PointerToRelocations;
  }

  DWORD GetPointerToLinenumbers() const
  {
    return data_.PointerToLinenumbers;
  }

  WORD GetNumberOfRelocations() const
  {
    return data_.NumberOfRelocations;
  }

  WORD GetNumberOfLinenumbers() const
  {
    return data_.NumberOfLinenumbers;
  }

  DWORD GetCharacteristics() const
  {
    return data_.Characteristics;
  }

  void SetName(std::string const& name)
  {
    if (name.size() > 8)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"New section name too large."});
    }

    std::copy(std::begin(name),
              std::end(name),
              reinterpret_cast<char*>(&data_.Name[0]));
    if (name.size() != 8)
    {
      data_.Name[name.size()] = '\0';
    }
  }

  void SetVirtualAddress(DWORD virtual_address)
  {
    data_.VirtualAddress = virtual_address;
  }

  void SetVirtualSize(DWORD virtual_size)
  {
    data_.Misc.VirtualSize = virtual_size;
  }

  void SetSizeOfRawData(DWORD size_of_raw_data)
  {
    data_.SizeOfRawData = size_of_raw_data;
  }

  void SetPointerToRawData(DWORD pointer_to_raw_data)
  {
    data_.PointerToRawData = pointer_to_raw_data;
  }

  void SetPointerToRelocations(DWORD pointer_to_relocations)
  {
    data_.PointerToRelocations = pointer_to_relocations;
  }

  void SetPointerToLinenumbers(DWORD pointer_to_linenumbers)
  {
    data_.PointerToLinenumbers = pointer_to_linenumbers;
  }

  void SetNumberOfRelocations(WORD number_of_relocations)
  {
    data_.NumberOfRelocations = number_of_relocations;
  }

  void SetNumberOfLinenumbers(WORD number_of_linenumbers)
  {
    data_.NumberOfLinenumbers = number_of_linenumbers;
  }

  void SetCharacteristics(DWORD characteristics)
  {
    data_.Characteristics = characteristics;
  }

private:
  template <typename SectionT> friend class SectionIterator;

  Process const* process_;
  PeFile const* pe_file_;
  std::uint8_t* base_;
  IMAGE_SECTION_HEADER data_ = IMAGE_SECTION_HEADER{};
  bool is_virtual_{};
};

inline bool operator==(Section const& lhs, Section const& rhs) noexcept
{
  return lhs.GetBase() == rhs.GetBase();
}

inline bool operator!=(Section const& lhs, Section const& rhs) noexcept
{
  return !(lhs == rhs);
}

inline bool operator<(Section const& lhs, Section const& rhs) noexcept
{
  return lhs.GetBase() < rhs.GetBase();
}

inline bool operator<=(Section const& lhs, Section const& rhs) noexcept
{
  return lhs.GetBase() <= rhs.GetBase();
}

inline bool operator>(Section const& lhs, Section const& rhs) noexcept
{
  return lhs.GetBase() > rhs.GetBase();
}

inline bool operator>=(Section const& lhs, Section const& rhs) noexcept
{
  return lhs.GetBase() >= rhs.GetBase();
}

inline std::ostream& operator<<(std::ostream& lhs, Section const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << rhs.GetBase();
  lhs.imbue(old);
  return lhs;
}

inline std::wostream& operator<<(std::wostream& lhs, Section const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << rhs.GetBase();
  lhs.imbue(old);
  return lhs;
}
}
