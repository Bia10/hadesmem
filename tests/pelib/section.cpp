// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#include <hadesmem/pelib/section.hpp>
#include <hadesmem/pelib/section.hpp>

#include <sstream>
#include <utility>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <boost/detail/lightweight_test.hpp>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <hadesmem/config.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/module.hpp>
#include <hadesmem/module_list.hpp>
#include <hadesmem/pelib/pe_file.hpp>
#include <hadesmem/pelib/nt_headers.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>

// TODO: Better tests. (In reference to assuming every module has at least one
// section, and that being the only constraint.)

void TestSection()
{
  hadesmem::Process const process(::GetCurrentProcessId());

  hadesmem::PeFile pe_file_1(
    process, ::GetModuleHandleW(nullptr), hadesmem::PeFileType::Image, 0);

  hadesmem::NtHeaders nt_headers_1(process, pe_file_1);

  BOOST_TEST(nt_headers_1.GetNumberOfSections() >= 1);

  hadesmem::Section section_1(process, pe_file_1, nullptr);

  hadesmem::Section section_2(section_1);
  BOOST_TEST_EQ(section_1, section_2);
  section_1 = section_2;
  BOOST_TEST_EQ(section_1, section_2);
  hadesmem::Section section_3(std::move(section_2));
  BOOST_TEST_EQ(section_3, section_1);
  section_2 = std::move(section_3);
  BOOST_TEST_EQ(section_1, section_2);

  hadesmem::ModuleList modules(process);
  for (auto const& mod : modules)
  {
    hadesmem::PeFile const cur_pe_file(
      process, mod.GetHandle(), hadesmem::PeFileType::Image, 0);

    // Assume every module has at least one section.
    hadesmem::NtHeaders cur_nt_headers(process, cur_pe_file);
    BOOST_TEST(cur_nt_headers.GetNumberOfSections() >= 1);
    hadesmem::Section cur_section(process, cur_pe_file, nullptr);

    auto const section_header_raw =
      hadesmem::Read<IMAGE_SECTION_HEADER>(process, cur_section.GetBase());

    cur_section.SetName(cur_section.GetName());
    cur_section.SetVirtualAddress(cur_section.GetVirtualAddress());
    cur_section.SetVirtualSize(cur_section.GetVirtualSize());
    cur_section.SetSizeOfRawData(cur_section.GetSizeOfRawData());
    cur_section.SetPointerToRawData(cur_section.GetPointerToRawData());
    cur_section.SetPointerToRelocations(cur_section.GetPointerToRelocations());
    cur_section.SetPointerToLinenumbers(cur_section.GetPointerToLinenumbers());
    cur_section.SetNumberOfRelocations(cur_section.GetNumberOfRelocations());
    cur_section.SetNumberOfLinenumbers(cur_section.GetNumberOfLinenumbers());
    cur_section.SetCharacteristics(cur_section.GetCharacteristics());
    cur_section.UpdateWrite();
    cur_section.UpdateRead();

    auto const section_header_raw_new =
      hadesmem::Read<IMAGE_SECTION_HEADER>(process, cur_section.GetBase());

    BOOST_TEST_EQ(std::memcmp(&section_header_raw,
                              &section_header_raw_new,
                              sizeof(section_header_raw)),
                  0);

    std::stringstream test_str_1;
    test_str_1.imbue(std::locale::classic());
    test_str_1 << cur_section;
    std::stringstream test_str_2;
    test_str_2.imbue(std::locale::classic());
    test_str_2 << cur_section.GetBase();
    BOOST_TEST_EQ(test_str_1.str(), test_str_2.str());
    if (mod.GetHandle() != GetModuleHandle(L"ntdll"))
    {
      hadesmem::PeFile const pe_file_ntdll(
        process, ::GetModuleHandleW(L"ntdll"), hadesmem::PeFileType::Image, 0);
      hadesmem::Section const section_ntdll(process, pe_file_ntdll, 0);
      std::stringstream test_str_3;
      test_str_3.imbue(std::locale::classic());
      test_str_3 << section_ntdll.GetBase();
      BOOST_TEST_NE(test_str_1.str(), test_str_3.str());
    }
  }
}

int main()
{
  TestSection();
  return boost::report_errors();
}
