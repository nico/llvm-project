#include "OutputSegment.h"
#include "lld/Common/Memory.h"

using namespace lld;
using namespace lld::macho;
using namespace llvm;

std::vector<OutputSegment *> macho::OutputSegments;

OutputSegment *macho::getOrCreateOutputSegment(StringRef Name, uint32_t Perms) {
  for (OutputSegment *OS : OutputSegments)
    if (OS->Name == Name)
      return OS;

  auto *OS = make<OutputSegment>();
  OS->Name = Name;
  OS->Perms = Perms;
  OutputSegments.push_back(OS);
  return OS;
}
