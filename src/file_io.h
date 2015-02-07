// This file contains the entities necessary doing disk IO for the working text file.
// For more details see texto.h.

#include "stdafx.h"

class FileDialog {
  plx::ComPtr<IShellItem> item_;

protected:
  void Show(const GUID& clsid, DWORD options, HWND owner) {
    plx::ComPtr<IFileDialog> dialog;
    auto hr = ::CoCreateInstance(clsid, NULL, 
                                 CLSCTX_INPROC_SERVER,
                                 __uuidof(dialog),
                                 reinterpret_cast<void **>(dialog.GetAddressOf()));
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);

    DWORD flags;
    hr = dialog->GetOptions(&flags);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    dialog->SetOptions(flags | options);
    hr = dialog->Show(owner);
    if (hr != S_OK)
      return;

    dialog->GetResult(item_.GetAddressOf());
  }

public:
  bool success() const {
    return item_ ? true : false;
  }

  plx::FilePath path() const {
    wchar_t* file_path = nullptr;
    auto hr = item_->GetDisplayName(SIGDN_FILESYSPATH, &file_path);
    if (hr != S_OK)
      throw plx::ComException(__LINE__, hr);
    plx::FilePath fp(file_path);
    ::CoTaskMemFree(file_path);
    return fp;
  }
};

class FileOpenDialog : public FileDialog {
public:
  FileOpenDialog(HWND owner) {
    Show(CLSID_FileOpenDialog, FOS_FORCEFILESYSTEM, owner);
  }
};

class FileSaveDialog : public FileDialog {
public:
  FileSaveDialog(HWND owner) {
    Show(CLSID_FileSaveDialog, FOS_FORCEFILESYSTEM, owner);
  }
};

class PlainTextFileIO {
  const plx::FilePath path_;
  const size_t io_size = 32 * 1024;

public:
  PlainTextFileIO(plx::FilePath& path) : path_(path) {
  }

  void save(const std::wstring text) {
    auto file = plx::File::Create(
        path_, 
        plx::FileParams::ReadWrite_SharedRead(CREATE_ALWAYS),
        plx::FileSecurity());
    file_to_disk(file, text);
  }

  std::unique_ptr<std::wstring> load() {
    auto file = plx::File::Create(
        path_, 
        plx::FileParams::ReadWrite_SharedRead(OPEN_EXISTING),
        plx::FileSecurity());
    // need to read the whole file at once because the UTF16 conversion fails if
    // we end up trying to convert in the middle of multi-byte sequence.
    return std::make_unique<std::wstring>(file_from_disk(file), false);
  }

private:
  void file_to_disk(plx::File& file, const std::wstring& contents) {
    auto block = plx::RangeFromString(contents);
    auto utf8 = plx::UTF8FromUTF16(block);
    file.write(plx::RangeFromString(utf8));
  }

  std::wstring file_from_disk(plx::File& file) {
    auto fsz = file.size_in_bytes();
    plx::Range<uint8_t> block(nullptr, fsz);
    auto heap = plx::HeapRange(block);
    auto bytes_read = file.read(block);
    // remove all CR so we only end up with LF.
    auto last = std::remove(block.start(), block.end(), '\r');
    return plx::UTF16FromUTF8(plx::Range<const uint8_t>(block.start(), last), false);
  }
};
