// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#pragma once

#include <cstddef>
#include <iosfwd>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include <windows.h>
#include <winnt.h>

#include <hadesmem/config.hpp>
#include <hadesmem/detail/str_conv.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/pelib/export_dir.hpp>
#include <hadesmem/pelib/nt_headers.hpp>
#include <hadesmem/pelib/pe_file.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>
#include <hadesmem/write.hpp>

// TODO: Ensure we properly support data exports. http://bit.ly/1Lu548u

// TODO: Add constructor to look up Export by name, optimized by using binary
// search.

// TODO: Is our naming of ordinal number vs procedure number correct/orthodox?
// Look into what other people/tools/documents call things.

namespace hadesmem
{
class Export
{
public:
  explicit Export(Process const& process,
                  PeFile const& pe_file,
                  WORD procedure_number)
    : process_{&process},
      pe_file_{&pe_file},
      procedure_number_{procedure_number}
  {
    ExportDir const export_dir{process, pe_file};

    auto const ordinal_base = static_cast<WORD>(export_dir.GetOrdinalBase());
    HADESMEM_DETAIL_ASSERT(procedure_number_ >= ordinal_base);
    ordinal_number_ = static_cast<WORD>(procedure_number_ - ordinal_base);
    if (ordinal_number_ >= export_dir.GetNumberOfFunctions())
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(Error{}
                                      << ErrorString{"Ordinal out of range."});
    }

    if (DWORD const num_names = export_dir.GetNumberOfNames())
    {
      WORD* const ptr_ordinals = static_cast<WORD*>(
        RvaToVa(process, pe_file, export_dir.GetAddressOfNameOrdinals()));
      DWORD* const ptr_names = static_cast<DWORD*>(
        RvaToVa(process, pe_file, export_dir.GetAddressOfNames()));

      if (ptr_ordinals && ptr_names)
      {
        std::vector<WORD> const name_ordinals =
          ReadVector<WORD>(process, ptr_ordinals, num_names);
        auto const name_ord_iter = std::find(
          std::begin(name_ordinals), std::end(name_ordinals), ordinal_number_);
        if (name_ord_iter != std::end(name_ordinals))
        {
          by_name_ = true;
          DWORD const name_rva =
            Read<DWORD>(process,
                        ptr_names + std::distance(std::begin(name_ordinals),
                                                  name_ord_iter));
          name_ = detail::CheckedReadString<char>(
            process, pe_file, RvaToVa(process, pe_file, name_rva));
        }
      }
    }

    DWORD* const ptr_functions = static_cast<DWORD*>(
      RvaToVa(process, pe_file, export_dir.GetAddressOfFunctions()));
    if (!ptr_functions)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"AddressOfFunctions invalid."});
    }
    rva_ptr_ = reinterpret_cast<DWORD*>(ptr_functions + ordinal_number_);
    DWORD const func_rva = Read<DWORD>(process, rva_ptr_);

    NtHeaders const nt_headers{process, pe_file};

    DWORD const export_dir_start =
      nt_headers.GetDataDirectoryVirtualAddress(PeDataDir::Export);
    DWORD const export_dir_end =
      export_dir_start + nt_headers.GetDataDirectorySize(PeDataDir::Export);

    // Check function RVA. If it lies inside the export dir region
    // then it's a forwarded export. Otherwise it's a regular RVA.
    if (func_rva > export_dir_start && func_rva < export_dir_end)
    {
      forwarded_ = true;
      forwarder_ = detail::CheckedReadString<char>(
        process, pe_file, RvaToVa(process, pe_file, func_rva));

      std::string::size_type const split_pos = forwarder_.rfind('.');
      if (split_pos != std::string::npos)
      {
        forwarder_split_ = std::make_pair(forwarder_.substr(0, split_pos),
                                          forwarder_.substr(split_pos + 1));
      }
      else
      {
        HADESMEM_DETAIL_THROW_EXCEPTION(
          Error{} << ErrorString{"Invalid forwarder string format."});
      }
    }
    else
    {
      rva_ = func_rva;
      va_ = RvaToVa(process, pe_file, func_rva, &virtual_va_);
    }
  }

  explicit Export(Process const&& process,
                  PeFile const& pe_file,
                  WORD procedure_number) = delete;

  explicit Export(Process const& process,
                  PeFile&& pe_file,
                  WORD procedure_number) = delete;

  explicit Export(Process const&& process,
                  PeFile&& pe_file,
                  WORD procedure_number) = delete;

  DWORD GetRva() const noexcept
  {
    return rva_;
  }

  DWORD* GetRvaPtr() const noexcept
  {
    return rva_ptr_;
  }

  PVOID GetVa() const noexcept
  {
    return va_;
  }

  std::string GetName() const
  {
    return name_;
  }

  WORD GetProcedureNumber() const noexcept
  {
    return procedure_number_;
  }

  WORD GetOrdinalNumber() const noexcept
  {
    return ordinal_number_;
  }

  bool ByName() const noexcept
  {
    return by_name_;
  }

  bool ByOrdinal() const noexcept
  {
    return !by_name_;
  }

  bool IsForwarded() const noexcept
  {
    return forwarded_;
  }

  std::string GetForwarder() const
  {
    return forwarder_;
  }

  std::string GetForwarderModule() const
  {
    return forwarder_split_.first;
  }

  std::string GetForwarderFunction() const
  {
    return forwarder_split_.second;
  }

  bool IsForwardedByOrdinal() const
  {
    return (GetForwarderFunction()[0] == '#');
  }

  WORD GetForwarderOrdinal() const
  {
    if (!IsForwardedByOrdinal())
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"Function is not exported by ordinal."});
    }

    try
    {
      std::string const forwarder_function{GetForwarderFunction()};
      return detail::StrToNum<WORD>(forwarder_function.substr(1));
    }
    catch (std::exception const& /*e*/)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"Invalid forwarder ordinal detected."});
    }
  }

  // TODO: Add setters for all fields.

  bool IsVirtualVa() const
  {
    return virtual_va_;
  }

private:
  Process const* process_;
  PeFile const* pe_file_;
  DWORD rva_{};
  DWORD* rva_ptr_{};
  void* va_{};
  std::string name_;
  std::string forwarder_;
  std::pair<std::string, std::string> forwarder_split_;
  WORD procedure_number_{};
  WORD ordinal_number_{};
  bool by_name_{};
  bool forwarded_{};
  bool virtual_va_{};
};

inline bool operator==(Export const& lhs, Export const& rhs) noexcept
{
  return lhs.GetProcedureNumber() == rhs.GetProcedureNumber();
}

inline bool operator!=(Export const& lhs, Export const& rhs) noexcept
{
  return !(lhs == rhs);
}

inline bool operator<(Export const& lhs, Export const& rhs) noexcept
{
  return lhs.GetProcedureNumber() < rhs.GetProcedureNumber();
}

inline bool operator<=(Export const& lhs, Export const& rhs) noexcept
{
  return lhs.GetProcedureNumber() <= rhs.GetProcedureNumber();
}

inline bool operator>(Export const& lhs, Export const& rhs) noexcept
{
  return lhs.GetProcedureNumber() > rhs.GetProcedureNumber();
}

inline bool operator>=(Export const& lhs, Export const& rhs) noexcept
{
  return lhs.GetProcedureNumber() >= rhs.GetProcedureNumber();
}

inline std::ostream& operator<<(std::ostream& lhs, Export const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << rhs.GetProcedureNumber();
  lhs.imbue(old);
  return lhs;
}

inline std::wostream& operator<<(std::wostream& lhs, Export const& rhs)
{
  std::locale const old = lhs.imbue(std::locale::classic());
  lhs << rhs.GetProcedureNumber();
  lhs.imbue(old);
  return lhs;
}
}
