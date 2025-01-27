/// \file PECOFF.cpp
/// \brief

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"

#include "revng/Model/Binary.h"
#include "revng/Support/Debug.h"

#include "BinaryImporterHelper.h"
#include "Importers.h"

using namespace llvm;

static Logger<> Log("pecoff-importer");

class PECOFFImporter : public BinaryImporterHelper {
private:
  TupleTree<model::Binary> &Model;
  const object::COFFObjectFile &TheBinary;

public:
  PECOFFImporter(TupleTree<model::Binary> &Model,
                 const object::COFFObjectFile &TheBinary) :
    Model(Model), TheBinary(TheBinary) {}

  Error import();
};

Error PECOFFImporter::import() {
  using namespace model;

  revng_assert(Model->Architecture != Architecture::Invalid);
  Architecture = Model->Architecture;

  auto PointerSize = Architecture::getPointerSize(Architecture);
  bool IsLittleEndian = Architecture::isLittleEndian(Architecture);

  if ((PointerSize != 4 and PointerSize != 8) or not IsLittleEndian)
    return createError("Only 32/64-bit little endian COFF files are supported");

  const object::pe32_header *PE32Header = TheBinary.getPE32Header();

  MetaAddress ImageBase = MetaAddress::invalid();
  if (PE32Header) {
    // TODO: ImageBase should aligned to 4kb pages, should we check that?
    ImageBase = fromPC(PE32Header->ImageBase);

    Model->EntryPoint = ImageBase + u64(PE32Header->AddressOfEntryPoint);
  } else {
    const object::pe32plus_header *PE32PlusHeader = TheBinary
                                                      .getPE32PlusHeader();
    if (not PE32PlusHeader)
      return createError("Invalid PE Header");

    // PE32+ Header
    ImageBase = fromPC(PE32PlusHeader->ImageBase);
    Model->EntryPoint = ImageBase + u64(PE32PlusHeader->AddressOfEntryPoint);
  }

  // Read sections
  for (const llvm::object::SectionRef &SecRef : TheBinary.sections()) {
    unsigned Id = TheBinary.getSectionID(SecRef);
    Expected<const object::coff_section *> SecOrErr = TheBinary.getSection(Id);
    if (not SecOrErr) {
      revng_log(Log,
                "Error in section with ID " << Id << ": "
                                            << SecOrErr.takeError());
      continue;
    }
    const object::coff_section *CoffRef = *SecOrErr;

    // VirtualSize might be larger than SizeOfRawData (extra data at the end of
    // the section) or viceversa (data mapped in memory but not present in
    // memory, e.g., .bss)
    uint64_t SegmentSize = std::min(CoffRef->VirtualSize,
                                    CoffRef->SizeOfRawData);

    MetaAddress Start = ImageBase + u64(CoffRef->VirtualAddress);
    Segment Segment({ Start, u64(CoffRef->VirtualSize) });

    Segment.StartOffset = CoffRef->PointerToRawData;
    Segment.FileSize = Segment.StartOffset + SegmentSize;

    Segment.IsReadable = CoffRef->Characteristics & COFF::IMAGE_SCN_MEM_READ;
    Segment.IsWriteable = CoffRef->Characteristics & COFF::IMAGE_SCN_MEM_WRITE;
    Segment.IsExecutable = CoffRef->Characteristics
                           & COFF::IMAGE_SCN_MEM_EXECUTE;
    Segment.verify(true);
    Model->Segments.insert(std::move(Segment));
  }

  return Error::success();
}

Error importPECOFF(TupleTree<model::Binary> &Model,
                   const object::COFFObjectFile &TheBinary,
                   uint64_t PreferredBaseAddress) {
  // TODO: use PreferredBaseAddress if PIC
  (void) PreferredBaseAddress;

  PECOFFImporter Importer(Model, TheBinary);
  return Importer.import();
}
