// DXBC containers for DDI shaders.
//
// The runtime hands the driver the tokenized program (the SHEX/SHDR payload) and signatures that carry only
// registers, masks, system values and component types. DXVK consumes DXBC containers, so the engine builds one:
// the program unchanged, signature chunks with semantic names derived from registers, and the DXBC hash that
// DXVK validates. Names are a pure function of (register, first component) or of the system value, so a VS
// output, the PS input it feeds and a stream-output declaration all agree without seeing each other.

#include <algorithm>
#include <cstring>

#include "ddi_device.h"

#include <dxbc/dxbc_container.h>
#include <dxbc/dxbc_parser.h>
#include <dxbc/dxbc_signature.h>

namespace dxbc_spv::dxbc {
  // Defined with external linkage in dxbc_container.cpp; not declared in its header.
  util::md5::Digest hashDxbcBinary(const void* data, size_t size);
}

namespace dxvk::ddi {

  using dxbc_spv::dxbc::OpCode;
  using dxbc_spv::dxbc::RegisterType;
  using dxbc_spv::dxbc::Signature;
  using dxbc_spv::dxbc::SignatureEntry;
  using dxbc_spv::dxbc::SignatureSysval;
  using dxbc_spv::ir::ScalarType;
  using dxbc_spv::util::FourCC;

  // D3D10_SB_TOKENIZED_PROGRAM_TYPE
  constexpr uint32_t ProgramPixel    = 0u;
  constexpr uint32_t ProgramGeometry = 2u;

  // Name of every non-system-value element; the semantic index encodes the register and first component.
  constexpr const char* RegisterSemantic = "BC250_R";

  struct SysvalInfo {
    const char*     name;
    SignatureSysval sysval;
    uint32_t        index;    // fixed semantic index, or ~0u to count elements of the same name
  };


  // D3D10_SB_NAME (the DDI's SystemValue) to the signature representation. The tokenized format names every
  // tessellation factor edge separately; signatures use one name and the semantic index.
  static SysvalInfo LookupSysval(uint32_t sbName) {
    switch (sbName) {
      case  1: return { "SV_Position",               SignatureSysval::ePosition,               0u  };
      case  2: return { "SV_ClipDistance",           SignatureSysval::eClipDistance,           ~0u };
      case  3: return { "SV_CullDistance",           SignatureSysval::eCullDistance,           ~0u };
      case  4: return { "SV_RenderTargetArrayIndex", SignatureSysval::eRenderTargetArrayIndex, 0u  };
      case  5: return { "SV_ViewportArrayIndex",     SignatureSysval::eViewportIndex,          0u  };
      case  6: return { "SV_VertexID",               SignatureSysval::eVertexId,               0u  };
      case  7: return { "SV_PrimitiveID",            SignatureSysval::ePrimitiveId,            0u  };
      case  8: return { "SV_InstanceID",             SignatureSysval::eInstanceId,             0u  };
      case  9: return { "SV_IsFrontFace",            SignatureSysval::eIsFrontFace,            0u  };
      case 10: return { "SV_SampleIndex",            SignatureSysval::eSampleIndex,            0u  };
      case 11: return { "SV_TessFactor",             SignatureSysval::eQuadEdgeTessFactor,     0u  };
      case 12: return { "SV_TessFactor",             SignatureSysval::eQuadEdgeTessFactor,     1u  };
      case 13: return { "SV_TessFactor",             SignatureSysval::eQuadEdgeTessFactor,     2u  };
      case 14: return { "SV_TessFactor",             SignatureSysval::eQuadEdgeTessFactor,     3u  };
      case 15: return { "SV_InsideTessFactor",       SignatureSysval::eQuadInsideTessFactor,   0u  };
      case 16: return { "SV_InsideTessFactor",       SignatureSysval::eQuadInsideTessFactor,   1u  };
      case 17: return { "SV_TessFactor",             SignatureSysval::eTriEdgeTessFactor,      0u  };
      case 18: return { "SV_TessFactor",             SignatureSysval::eTriEdgeTessFactor,      1u  };
      case 19: return { "SV_TessFactor",             SignatureSysval::eTriEdgeTessFactor,      2u  };
      case 20: return { "SV_InsideTessFactor",       SignatureSysval::eTriInsideTessFactor,    0u  };
      case 21: return { "SV_TessFactor",             SignatureSysval::eLineDetailTessFactor,   0u  };
      case 22: return { "SV_TessFactor",             SignatureSysval::eLineDensityTessFactor,  1u  };
      default: return { nullptr,                     SignatureSysval::eNone,                   0u  };
    }
  }


  // D3D10_SB_REGISTER_COMPONENT_TYPE and D3D11_SB_OPERAND_MIN_PRECISION to the IR scalar type.
  static ScalarType LookupScalarType(uint32_t componentType, uint32_t minPrecision) {
    switch (componentType) {
      case 1: // UINT32
        return minPrecision == 5u ? ScalarType::eMinU16 : ScalarType::eU32;
      case 2: // SINT32
        return minPrecision == 4u ? ScalarType::eMinI16 : ScalarType::eI32;
      default: // FLOAT32, or UNKNOWN which the runtime uses for untyped registers
        return (minPrecision == 1u || minPrecision == 2u) ? ScalarType::eMinF16 : ScalarType::eF32;
    }
  }


  static uint32_t FirstComponent(uint32_t mask) {
    for (uint32_t i = 0u; i < 4u; i++) {
      if (mask & (1u << i))
        return i;
    }

    return 0u;
  }


  static uint32_t ComponentCount(uint32_t mask) {
    return bit::popcnt(mask & 0xfu);
  }


  struct NamedEntry {
    std::string name;
    uint32_t    index;
    uint32_t    reg;
    uint32_t    stream;
    uint32_t    mask;
  };


  // Builds one signature chunk. Records the names given to each entry for stream-output lookups.
  static bool BuildSignature(
          FourCC                        Tag,
    const BC250_DXVK_SIGNATURE&         Ddi,
          bool                          IsInput,
          bool                          IsPixelOutput,
          Signature*                    pSignature,
          std::vector<NamedEntry>*      pNames) {
    *pSignature = Signature(Tag);

    uint32_t clipCount = 0u;
    uint32_t cullCount = 0u;

    for (uint32_t i = 0u; i < Ddi.NumEntries; i++) {
      const BC250_DXVK_SIGNATURE_ENTRY& e = Ddi.Entries[i];

      // Registerless entries (depth, coverage) are declared as built-ins by the program itself
      if (e.Register == ~0u)
        continue;

      std::string     name;
      uint32_t        index  = 0u;
      SignatureSysval sysval = SignatureSysval::eNone;

      if (IsPixelOutput && !e.SystemValue) {
        name   = "SV_Target";
        index  = e.Register;
        sysval = SignatureSysval::eTarget;
      } else if (e.SystemValue) {
        SysvalInfo sv = LookupSysval(e.SystemValue);

        if (!sv.name) {
          Logger::err(str::format("bc250dxvk: Unsupported system value ", e.SystemValue, " in signature"));
          return false;
        }

        name   = sv.name;
        sysval = sv.sysval;
        index  = sv.index;

        if (sysval == SignatureSysval::eClipDistance)
          index = clipCount++;
        else if (sysval == SignatureSysval::eCullDistance)
          index = cullCount++;
      } else {
        name  = RegisterSemantic;
        index = e.Register * 4u + FirstComponent(e.Mask);
      }

      // Inputs: every declared component counts as read. Outputs: no component is marked as never written.
      uint32_t mask = e.Mask & 0xfu;
      uint32_t componentMask = mask | (IsInput ? (mask << 8u) : 0u);

      pSignature->add(SignatureEntry(name.c_str(), index, int32_t(e.Register), e.Stream,
        componentMask, sysval, LookupScalarType(e.ComponentType, e.MinPrecision)));

      if (pNames)
        pNames->push_back({ name, index, e.Register, e.Stream, mask });
    }

    return true;
  }


  static std::vector<uint8_t> WriteChunk(const Signature& signature) {
    dxbc_spv::util::ByteWriter writer;
    signature.write(writer);

    auto data = std::move(writer).extract();
    return std::vector<uint8_t>(data.begin(), data.end());
  }


  // The runtime's signature entries carry no stream (D3D11_1DDIARG_SIGNATURE_ENTRY), but a gs_5_0 program
  // declares each output inside the dcl_stream block it belongs to. Each entry takes the first stream whose
  // declarations cover its register and components and have not given those components to an earlier entry.
  // A register used on two streams has one entry per stream, and the runtime lists them in stream order.
  static void DeriveOutputStreams(
    const std::vector<uint8_t>&                     ProgramChunk,
          std::vector<BC250_DXVK_SIGNATURE_ENTRY>&  Entries) {
    struct Declared {
      uint32_t reg;
      uint32_t mask;
      uint32_t stream;
      uint32_t claimed;
    };

    std::vector<Declared> declared;

    dxbc_spv::dxbc::Parser parser(dxbc_spv::util::ByteReader(ProgramChunk.data(), ProgramChunk.size()));
    uint32_t stream = 0u;

    while (parser) {
      auto op = parser.parseInstruction();

      if (!op)
        break;

      if (!op.getDstCount())
        continue;

      auto opCode = op.getOpToken().getOpCode();
      const auto& dst = op.getDst(0u);

      if (opCode == OpCode::eDclStream && dst.getRegisterType() == RegisterType::eStream) {
        stream = dst.getIndex(0u);
      } else if ((opCode == OpCode::eDclOutput || opCode == OpCode::eDclOutputSgv || opCode == OpCode::eDclOutputSiv)
              && dst.getRegisterType() == RegisterType::eOutput && dst.getIndexDimensions()) {
        declared.push_back({ dst.getIndex(dst.getIndexDimensions() - 1u),
          uint32_t(uint8_t(dst.getWriteMask())), stream, 0u });
      }
    }

    for (auto& e : Entries) {
      uint32_t mask = e.Mask & 0xfu;

      if (e.Register == ~0u)
        continue;

      for (auto& d : declared) {
        if (d.reg == e.Register && (d.mask & mask) && !(d.claimed & mask)) {
          e.Stream = BYTE(d.stream);
          d.claimed |= mask;
          break;
        }
      }
    }
  }


  // vs_4_0 with a single ret: version token, length token, instruction
  static const UINT EmptyVertexProgram[] = { 0x00010040u, 3u, 0x0100003eu };


  uint32_t GetProgramType(const UINT* pCode) {
    return pCode ? (pCode[0] >> 16u) : ~0u;
  }


  HRESULT BuildShaderContainer(
    const BC250_DXVK_SHADER_DESC*             pDesc,
          std::vector<uint8_t>*               pContainer,
          std::vector<D3D11_SO_DECLARATION_ENTRY>* pSoEntries,
          std::vector<std::string>*           pSoNames) {
    if (!pDesc)
      return E_INVALIDARG;

    const UINT* pCode = pDesc->Code;
    BC250_DXVK_SIGNATURE input = pDesc->Input;
    BC250_DXVK_SIGNATURE output = pDesc->Output;

    // Stream output of the vertex or domain program, with no geometry program, may arrive without code. An
    // empty vertex program then carries the output signature; DXVK builds a pass-through geometry shader from
    // that signature, as it does for a vertex or domain program passed as geometry shader code.
    if (!pCode && pDesc->StreamOutput) {
      pCode = EmptyVertexProgram;
      input = { };

      if (!output.NumEntries)
        output = pDesc->Input;
    }

    if (!pCode)
      return E_INVALIDARG;

    uint32_t version = pCode[0];
    uint32_t tokenCount = pCode[1];
    uint32_t programType = version >> 16u;
    uint32_t major = (version >> 4u) & 0xfu;

    if (tokenCount < 2u)
      return E_INVALIDARG;

    bool isPixel = programType == ProgramPixel;
    bool hasStreams = programType == ProgramGeometry && major >= 5u;

    // Program chunk: tag, byte size, tokens unchanged
    std::vector<uint8_t> code(8u + tokenCount * sizeof(UINT));
    std::memcpy(&code[0], major >= 5u ? "SHEX" : "SHDR", 4u);
    uint32_t codeSize = tokenCount * sizeof(UINT);
    std::memcpy(&code[4], &codeSize, 4u);
    std::memcpy(&code[8], pCode, codeSize);

    // Streams of a gs_5_0 output signature, unless the shell passed them (ENTRY2 from a runtime that fills it)
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> streamOutputs;

    if (hasStreams && output.NumEntries && std::none_of(output.Entries, output.Entries + output.NumEntries,
        [] (const BC250_DXVK_SIGNATURE_ENTRY& e) { return e.Stream != 0u; })) {
      streamOutputs.assign(output.Entries, output.Entries + output.NumEntries);
      DeriveOutputStreams(code, streamOutputs);
      output.Entries = streamOutputs.data();
    }

    Signature isgn, osgn, pcsg;
    std::vector<NamedEntry> outputNames;

    if (!BuildSignature(FourCC("ISGN"), input, true, false, &isgn, nullptr)
     || !BuildSignature(FourCC(hasStreams ? "OSG5" : "OSGN"), output, false, isPixel, &osgn, &outputNames))
      return E_INVALIDARG;

    bool hasPatchConstants = pDesc->PatchConstant.NumEntries != 0u;

    // The patch constant signature is the hull shader's output and the domain shader's input
    if (hasPatchConstants && !BuildSignature(FourCC("PCSG"), pDesc->PatchConstant,
        programType == 4u /* domain */, false, &pcsg, nullptr))
      return E_INVALIDARG;

    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(WriteChunk(isgn));
    chunks.push_back(WriteChunk(osgn));

    if (hasPatchConstants)
      chunks.push_back(WriteChunk(pcsg));

    chunks.push_back(std::move(code));

    // Header: magic, hash, version 1, file size, chunk count, chunk offsets
    uint32_t chunkCount = uint32_t(chunks.size());
    uint32_t headerSize = 32u + 4u * chunkCount;
    uint32_t fileSize = headerSize;

    for (const auto& c : chunks)
      fileSize += uint32_t(c.size());

    auto& out = *pContainer;
    out.assign(fileSize, 0u);
    std::memcpy(&out[0], "DXBC", 4u);

    uint32_t one = 1u;
    std::memcpy(&out[20], &one, 4u);
    std::memcpy(&out[24], &fileSize, 4u);
    std::memcpy(&out[28], &chunkCount, 4u);

    uint32_t offset = headerSize;

    for (uint32_t i = 0u; i < chunkCount; i++) {
      std::memcpy(&out[32u + 4u * i], &offset, 4u);
      std::memcpy(&out[offset], chunks[i].data(), chunks[i].size());
      offset += uint32_t(chunks[i].size());
    }

    auto digest = dxbc_spv::dxbc::hashDxbcBinary(out.data(), out.size());
    std::memcpy(&out[4], digest.data.data(), digest.data.size());

    // Stream output declaration in D3D11 terms, naming the output elements built above. Holes have
    // RegisterIndex ~0u (as in Mesa's d3d10umd) and skip the components in their mask.
    if (pSoEntries && pDesc->StreamOutput) {
      const BC250_DXVK_STREAM_OUTPUT& so = *pDesc->StreamOutput;

      pSoEntries->clear();
      pSoNames->clear();
      pSoNames->reserve(so.NumEntries);

      for (uint32_t i = 0u; i < so.NumEntries; i++) {
        const BC250_DXVK_SO_ENTRY& e = so.Entries[i];

        D3D11_SO_DECLARATION_ENTRY d = { };
        d.Stream = e.Stream;
        d.OutputSlot = BYTE(e.OutputSlot);
        d.ComponentCount = BYTE(ComponentCount(e.RegisterMask));

        if (e.RegisterIndex != ~0u) {
          uint32_t first = FirstComponent(e.RegisterMask);

          auto match = std::find_if(outputNames.begin(), outputNames.end(), [&] (const NamedEntry& n) {
            return n.reg == e.RegisterIndex && n.stream == (hasStreams ? e.Stream : 0u) && (n.mask & (1u << first));
          });

          if (match == outputNames.end()) {
            Logger::err(str::format("bc250dxvk: Stream output register ", e.RegisterIndex, " not in output signature"));
            return E_INVALIDARG;
          }

          pSoNames->push_back(match->name);
          d.SemanticName = pSoNames->back().c_str();
          d.SemanticIndex = match->index;
          d.StartComponent = BYTE(first - FirstComponent(match->mask));
        }

        pSoEntries->push_back(d);
      }
    }

    return S_OK;
  }

}
