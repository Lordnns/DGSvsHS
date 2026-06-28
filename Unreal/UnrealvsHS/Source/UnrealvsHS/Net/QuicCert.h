

#pragma once

#include <cstdint>
#include <cstddef>

namespace UnrealvsHS::Net
{

	uint8_t* GenerateSelfSignedPkcs12(int32_t* OutLength);
	
	void FreePkcs12Buffer(uint8_t* Buffer);

	bool GenerateSelfSignedPemFiles(const char* CertPath, const char* KeyPath);
}
