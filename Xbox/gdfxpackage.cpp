#include "gdfxpackage.h"
#include <algorithm>

GdfxPackage::GdfxPackage(std::vector<std::string> FilePath, StfsPackage *package)
{
//    std::vector<std::wstring> paths;
//    for (int i = 0; i < FilePath.size(); i++)
//    {
//        std::wstring temp(FilePath[i].length(),L' ');
//        std::copy(FilePath[i].begin(), FilePath[i].end(), temp.begin());
//        paths.push_back(temp);
//    }
    fileStream = (Streams::IStream*)new Streams::xMultiFileStream(FilePath);
    this->package = package;
    Initialize();
}

GdfxPackage::GdfxPackage(Streams::IStream *FileStream, StfsPackage *package)
{
    fileStream = FileStream;
    this->package = package;
    Initialize();
}

void GdfxPackage::Close( void )
{
    fileStream->Close();
    CloseFolder(RootDirectory);
    RootDirectory = NULL;
}

void GdfxPackage::CloseFolder(Folder *f )
{
    // fuck you apple for not updating gcc, so I can't use C++11 features.
    // for ( Folder *folder : f->Folders )
    for (int i = 0; i < f->Folders.size(); i++)
        CloseFolder(f->Folders[i]);
    f->Folders.clear();
    f->Files.clear();
    delete f;
}

void GdfxPackage::Initialize( void )
{
    // Check the magic
    std::string magic = Magic();
    if (magic.compare("MICROSOFT*XBOX*MEDIA"))
        throw xException(std::string("GDFX volume has a bad magic"), 0xBAADF00D);
    RootBlockIsDvdSignature = false;
    DataBlockCount = package->DataBlockCount();
    DataBlockOffset = package->DataBlockOffset();
    BaseOffset = (UINT64)(DataBlockOffset << 12) - 0x1000;
    // Let's check to see if the root directory is the
    // SDK tool's signature block
    fileStream->SetPosition(SeekToBlock(RootBlock()));
    std::string signature = fileStream->ReadString(0x18);
    if (!signature.compare("XBOX_DVD_LAYOUT_TOOL_SIG"))
        RootBlockIsDvdSignature = true;
    // Read the root dir
    RootDirectory = new Folder();
    std::vector<Dirent> RootDirents = LoadDirents(RootBlock(), RootSize());
    RootDirectory->DirentsRead = true;
    for (int i = 0; i < RootDirents.size(); i++) // fucking C++11 bullshit apple. Dirent dirent : RootDirents)
    {
        Dirent dirent = RootDirents[i];
        if (dirent.Flag & Attributes::DIRECTORY)
        {
            Folder* f = new Folder();
            f->DirentsRead = false;
            f->Entry = dirent;
            f->FullPath = dirent.FileName;
            LoadFolderDirents(f);
            RootDirectory->Folders.push_back(f);
        }
        else
            RootDirectory->Files.push_back(dirent);
    }
}

std::string GdfxPackage::Magic( void )
{
    // Seek to the header
    fileStream->SetPosition(0x2000);
    // Read the string there
    std::string magic = fileStream->ReadString(0x14);
    return magic;
}

DWORD GdfxPackage::RootBlock( void )
{
    // Seek to the header + 0x14
    fileStream->SetPosition(0x2014);
    fileStream->SetEndianness(Streams::Little);
    DWORD Block = fileStream->ReadUInt32();
    fileStream->SetEndianness(Streams::Big);
    return Block;
}

DWORD GdfxPackage::RootSize( void )
{
    // Seek to the header + 0x18
    fileStream->SetPosition(0x2018);
    fileStream->SetEndianness(Streams::Little);
    DWORD Size = fileStream->ReadUInt32();
    fileStream->SetEndianness(Streams::Big);
    return Size;
}

std::vector<Dirent> GdfxPackage::LoadDirents(DWORD Block, DWORD Size)
{
    // Initialize our return vector
    std::vector<Dirent> returnVector;
    if (Size == 0)
        return returnVector;
    int Blocks = Size / BLOCK_SIZE;
    if (!Blocks)
        Blocks++;
    for (int i = 0; i < Blocks; i++)
    {
        // Set the IO's position
        fileStream->SetPosition(SeekToBlock(Block));
        UINT64 BlockOffset = fileStream->Position();
        // Check to see if we've reached the end of the block yet
        UINT32 EndOfBlock = fileStream->ReadUInt32();
        fileStream->SetPosition(fileStream->Position() - 4);
        // Loop until we either hit the end of the dirents (0xFFFF), or the end of the block
        while (EndOfBlock != 0xFFFFFFFF && fileStream->Position() < BlockOffset + BLOCK_SIZE)
        {
            Dirent d = {0};
            // Skip the first 2 shorts -- no idea what they are
            fileStream->SetPosition(fileStream->Position() + 0x4);
            fileStream->SetEndianness(Streams::Little);
            d.Offset = fileStream->ReadUInt32();
            d.Length = fileStream->ReadUInt32();
            fileStream->SetEndianness(Streams::Big);
            d.Flag = fileStream->ReadByte();
            d.FileNameLength = fileStream->ReadByte();
            d.FileName = fileStream->ReadString(d.FileNameLength);

            returnVector.push_back(d);

            BYTE result = 0x0;
            int reads = 0;
            for (int i = 0; i < 4; i++, reads++)
            {
                result = fileStream->ReadByte();
                if (result != 0xFF)
                    break;
            }
            if (reads == 4)
                break;
            fileStream->SetPosition(fileStream->Position() - 1);

            EndOfBlock = fileStream->ReadUInt32();
            fileStream->SetPosition(fileStream->Position() - 4);
        }
    }
    return returnVector;
}

UINT64 GdfxPackage::SeekToBlock(UINT32 Block)
{
    // Calculate the raw offset (i.e. offset if there were no hash tables)
    UINT64 RawOffset = (UINT64)Block * BLOCK_SIZE;
    RawOffset -= BaseOffset;
    // Now that we have the raw offset, determine how many file start hash tables are between 0 and here
    UINT64 HashTables = RawOffset + (RawOffset % FILE_MAX_SIZE);
    HashTables /= FILE_MAX_SIZE;
    // If the number was too small (offset is in the first file), increment it
    if (!HashTables)
        HashTables++;
    // Add the number of hash tables * 0x2000 to the RawOffset
    RawOffset += HashTables * 0x2000;
    // Now calculate the number of hash tables IN the files there are
    HashTables = RawOffset / DATA_BETWEEN_TABLES;
    RawOffset += HashTables * HASH_TABLE_SIZE;
    if (!RootBlockIsDvdSignature)
        RawOffset -= 0x1000;
    return RawOffset;
}

void GdfxPackage::LoadFolderDirents(Folder *f, bool Recursive)
{
    if (f->DirentsRead)
        return;
    std::vector<Dirent> Dirents = LoadDirents(f->Entry.Offset, f->Entry.Length);
    f->DirentsRead = true;
    for (int i = 0; i < Dirents.size(); i++)// fuck you for (Dirent dirent : Dirents)
    {
        Dirent dirent = Dirents[i];
        if (dirent.Flag & Attributes::DIRECTORY)
        {
            Folder* folder = new Folder();
            folder->DirentsRead = false;
            folder->Entry = dirent;
            folder->FullPath = dirent.FileName;
            if (Recursive)
                LoadFolderDirents(folder);
            f->Folders.push_back(folder);
        }
        else
            f->Files.push_back(dirent);
    }
}
