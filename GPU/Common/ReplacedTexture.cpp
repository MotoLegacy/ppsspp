// Copyright (c) 2016- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>

#include "ppsspp_config.h"

#include <png.h>

#include "ext/basis_universal/basisu_transcoder.h"
#include "ext/basis_universal/basisu_file_headers.h"

#include "GPU/Common/ReplacedTexture.h"
#include "GPU/Common/TextureReplacer.h"

#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Format/DDSLoad.h"
#include "Common/Data/Format/ZIMLoad.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/Thread/ParallelLoop.h"
#include "Common/Thread/Waitable.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"

#define MK_FOURCC(str) (str[0] | ((uint8_t)str[1] << 8) | ((uint8_t)str[2] << 16) | ((uint8_t)str[3] << 24))

static ReplacedImageType IdentifyMagic(const uint8_t magic[4]) {
	if (memcmp((const char *)magic, "ZIMG", 4) == 0)
		return ReplacedImageType::ZIM;
	else if (magic[0] == 0x89 && strncmp((const char *)&magic[1], "PNG", 3) == 0)
		return ReplacedImageType::PNG;
	else if (memcmp((const char *)magic, "DDS ", 4) == 0)
		return ReplacedImageType::DDS;
	else if (magic[0] == 's' && magic[1] == 'B') {
		uint16_t ver = magic[2] | (magic[3] << 8);
		if (ver >= 0x10) {
			return ReplacedImageType::BASIS;
		}
	} else if (memcmp((const char *)magic, "\xabKTX", 4) == 0) {
		// Technically, should read 12 bytes here, but this'll do.
		return ReplacedImageType::KTX2;
	}
	return ReplacedImageType::INVALID;
}

static ReplacedImageType Identify(VFSBackend *vfs, VFSOpenFile *openFile, std::string *outMagic) {
	uint8_t magic[4];
	if (vfs->Read(openFile, magic, 4) != 4) {
		*outMagic = "FAIL";
		return ReplacedImageType::INVALID;
	}
	// Turn the signature into a readable string that we can display in an error message.
	*outMagic = std::string((const char *)magic, 4);
	for (int i = 0; i < outMagic->size(); i++) {
		if ((s8)(*outMagic)[i] < 32) {
			(*outMagic)[i] = '_';
		}
	}
	vfs->Rewind(openFile);
	return IdentifyMagic(magic);
}

class ReplacedTextureTask : public Task {
public:
	ReplacedTextureTask(VFSBackend *vfs, ReplacedTexture &tex, LimitedWaitable *w) : vfs_(vfs), tex_(tex), waitable_(w) {}

	TaskType Type() const override { return TaskType::IO_BLOCKING; }
	TaskPriority Priority() const override { return TaskPriority::NORMAL; }

	void Run() override {
		tex_.Prepare(vfs_);
		waitable_->Notify();
	}

private:
	VFSBackend *vfs_;
	ReplacedTexture &tex_;
	LimitedWaitable *waitable_;
};

ReplacedTexture::~ReplacedTexture() {
	if (threadWaitable_) {
		SetState(ReplacementState::CANCEL_INIT);

		std::unique_lock<std::mutex> lock(mutex_);
		threadWaitable_->WaitAndRelease();
		threadWaitable_ = nullptr;
	}

	for (auto &level : levels_) {
		vfs_->ReleaseFile(level.fileRef);
		level.fileRef = nullptr;
	}
}

void ReplacedTexture::PurgeIfOlder(double t) {
	if (threadWaitable_ && !threadWaitable_->WaitFor(0.0))
		return;
	if (lastUsed_ >= t)
		return;

	if (levelData_ && levelData_->lastUsed < t) {
		// We have to lock since multiple textures might reference this same data.
		std::lock_guard<std::mutex> guard(levelData_->lock);
		levelData_->data.clear();
		// This means we have to reload.  If we never purge any, there's no need.
		SetState(ReplacementState::POPULATED);
	}
}

// This can only return true if ACTIVE or NOT_FOUND.
bool ReplacedTexture::IsReady(double budget) {
	_assert_(vfs_ != nullptr);

	double now = time_now_d();

	switch (State()) {
	case ReplacementState::ACTIVE:
	case ReplacementState::NOT_FOUND:
		if (threadWaitable_) {
			if (!threadWaitable_->WaitFor(budget)) {
				lastUsed_ = now;
				return false;
			}
			// Successfully waited! Can get rid of it.
			threadWaitable_->WaitAndRelease();
			threadWaitable_ = nullptr;
			if (levelData_) {
				levelData_->lastUsed = now;
			}
		}
		lastUsed_ = now;
		return true;
	case ReplacementState::UNINITIALIZED:
		// _dbg_assert_(false);
		return false;
	case ReplacementState::CANCEL_INIT:
	case ReplacementState::PENDING:
		return false;
	case ReplacementState::POPULATED:
		// We're gonna need to spawn a task.
		break;
	}

	lastUsed_ = now;

	// Let's not even start a new texture if we're already behind.
	if (budget < 0.0)
		return false;

	_assert_(!threadWaitable_);
	threadWaitable_ = new LimitedWaitable();
	SetState(ReplacementState::PENDING);
	g_threadManager.EnqueueTask(new ReplacedTextureTask(vfs_, *this, threadWaitable_));
	if (threadWaitable_->WaitFor(budget)) {
		// If we successfully wait here, we're done. The thread will set state accordingly.
		_assert_(State() == ReplacementState::ACTIVE || State() == ReplacementState::NOT_FOUND || State() == ReplacementState::CANCEL_INIT);
		return true;
	}
	// Still pending on thread.
	return false;
}

void ReplacedTexture::FinishPopulate(ReplacementDesc *desc) {
	logId_ = desc->logId;
	levelData_ = desc->cache;
	desc_ = desc;
	SetState(ReplacementState::POPULATED);

	// TODO: What used to be here is now done on the thread task.
}

void ReplacedTexture::Prepare(VFSBackend *vfs) {
	this->vfs_ = vfs;

	std::unique_lock<std::mutex> lock(mutex_);

	_assert_msg_(levelData_ != nullptr, "Level cache not set");

	// We must lock around access to levelData_ in case two textures try to load it at once.
	std::lock_guard<std::mutex> guard(levelData_->lock);

	fmt = Draw::DataFormat::UNDEFINED;

	Draw::DataFormat pixelFormat;
	LoadLevelResult result = LoadLevelResult::LOAD_ERROR;
	if (desc_->filenames.empty()) {
		result = LoadLevelResult::DONE;
	}
	for (int i = 0; i < std::min(MAX_REPLACEMENT_MIP_LEVELS, (int)desc_->filenames.size()); ++i) {
		if (State() == ReplacementState::CANCEL_INIT) {
			break;
		}

		if (desc_->filenames[i].empty()) {
			// Out of valid mip levels.  Bail out.
			break;
		}

		VFSFileReference *fileRef = vfs_->GetFile(desc_->filenames[i].c_str());
		if (!fileRef) {
			// If the file doesn't exist, let's just bail immediately here.
			// Mark as DONE, not error.
			result = LoadLevelResult::DONE;
			break;
		}

		if (i == 0) {
			fmt = Draw::DataFormat::R8G8B8A8_UNORM;
		}

		result = LoadLevelData(fileRef, desc_->filenames[i], i, &pixelFormat);
		if (result == LoadLevelResult::DONE) {
			// Loaded all the levels we're gonna get.
			fmt = pixelFormat;
			break;
		} else if (result == LoadLevelResult::CONTINUE) {
			if (i == 0) {
				fmt = pixelFormat;
			} else {
				if (fmt != pixelFormat) {
					ERROR_LOG(G3D, "Replacement mipmap %d doesn't have the same pixel format as mipmap 0. Stopping.", i);
					break;
				}
			}
		} else {
			// Error state.
			break;
		}
	}

	if (levels_.empty()) {
		// No replacement found.
		std::string name = TextureReplacer::HashName(desc_->cachekey, desc_->hash, 0);
		if (result == LoadLevelResult::LOAD_ERROR) {
			WARN_LOG(G3D, "Failed to load replacement texture '%s'", name.c_str());
		}
		SetState(ReplacementState::NOT_FOUND);
		levelData_ = nullptr;
		delete desc_;
		desc_ = nullptr;
		return;
	}

	levelData_->fmt = fmt;
	SetState(ReplacementState::ACTIVE);

	delete desc_;
	desc_ = nullptr;

	if (threadWaitable_)
		threadWaitable_->Notify();
}

inline uint32_t RoundUpTo4(uint32_t value) {
	return (value + 3) & ~3;
}

// Returns true if Prepare should keep calling this to load more levels.
ReplacedTexture::LoadLevelResult ReplacedTexture::LoadLevelData(VFSFileReference *fileRef, const std::string &filename, int mipLevel, Draw::DataFormat *pixelFormat) {
	bool good = false;

	if (levelData_->data.size() <= mipLevel) {
		levelData_->data.resize(mipLevel + 1);
	}

	ReplacedTextureLevel level;
	size_t fileSize;
	VFSOpenFile *openFile = vfs_->OpenFileForRead(fileRef, &fileSize);
	if (!openFile) {
		// File missing, no more levels. This is alright.
		return LoadLevelResult::DONE;
	}

	std::string magic;
	ReplacedImageType imageType = Identify(vfs_, openFile, &magic);

	bool ddsDX10 = false;
	int numMips = 1;

	if (imageType == ReplacedImageType::KTX2) {
		KTXHeader header;
		good = vfs_->Read(openFile, &header, sizeof(header)) == sizeof(header);

		level.w = header.pixelWidth;
		level.h = header.pixelHeight;
		numMips = header.levelCount;

		// Additional quick checks
		good = good && header.layerCount <= 1;
	} else if (imageType == ReplacedImageType::BASIS) {
		WARN_LOG(G3D, "The basis texture format is not supported. Use KTX2 (basisu texture.png -uastc -ktx2 -mipmap)");

		// We simply don't support basis files currently.
		good = false;
	} else if (imageType == ReplacedImageType::DDS) {
		DDSHeader header;
		DDSHeaderDXT10 header10{};
		good = vfs_->Read(openFile, &header, sizeof(header)) == sizeof(header);

		*pixelFormat = Draw::DataFormat::UNDEFINED;
		u32 format;
		if (good && (header.ddspf.dwFlags & DDPF_FOURCC)) {
			char *fcc = (char *)&header.ddspf.dwFourCC;
			// INFO_LOG(G3D, "DDS fourcc: %c%c%c%c", fcc[0], fcc[1], fcc[2], fcc[3]);
			if (header.ddspf.dwFourCC == MK_FOURCC("DX10")) {
				ddsDX10 = true;
				good = good && vfs_->Read(openFile, &header10, sizeof(header10)) == sizeof(header10);
				format = header10.dxgiFormat;
				switch (format) {
				case 98: // DXGI_FORMAT_BC7_UNORM:
				case 99: // DXGI_FORMAT_BC7_UNORM_SRGB:
					if (!desc_->formatSupport.bc7) {
						WARN_LOG(G3D, "BC1-3 formats not supported, skipping texture");
						good = false;
					}
					*pixelFormat = Draw::DataFormat::BC7_UNORM_BLOCK;
					break;
				default:
					WARN_LOG(G3D, "DXGI pixel format %d not supported.", header10.dxgiFormat);
					good = false;
				}
			} else {
				if (!desc_->formatSupport.bc123) {
					WARN_LOG(G3D, "BC1-3 formats not supported");
					good = false;
				}
				format = header.ddspf.dwFourCC;
				// OK, there are a number of possible formats we might have ended up with. We choose just a few
				// to support for now.
				switch (format) {
				case MK_FOURCC("DXT1"):
					*pixelFormat = Draw::DataFormat::BC1_RGBA_UNORM_BLOCK;
					break;
				case MK_FOURCC("DXT3"):
					*pixelFormat = Draw::DataFormat::BC2_UNORM_BLOCK;
					break;
				case MK_FOURCC("DXT5"):
					*pixelFormat = Draw::DataFormat::BC3_UNORM_BLOCK;
					break;
				default:
					ERROR_LOG(G3D, "DDS pixel format not supported.");
					good = false;
				}
			}
		} else if (good) {
			ERROR_LOG(G3D, "DDS non-fourCC format not supported.");
			good = false;
		}

		level.w = header.dwWidth;
		level.h = header.dwHeight;
		numMips = header.dwMipMapCount;
	} else if (imageType == ReplacedImageType::ZIM) {
		uint32_t ignore = 0;
		struct ZimHeader {
			uint32_t magic;
			uint32_t w;
			uint32_t h;
			uint32_t flags;
		} header;
		good = vfs_->Read(openFile, &header, sizeof(header)) == sizeof(header);
		level.w = header.w;
		level.h = header.h;
		good = good && (header.flags & ZIM_FORMAT_MASK) == ZIM_RGBA8888;
		*pixelFormat = Draw::DataFormat::R8G8B8A8_UNORM;
	} else if (imageType == ReplacedImageType::PNG) {
		PNGHeaderPeek headerPeek;
		good = vfs_->Read(openFile, &headerPeek, sizeof(headerPeek)) == sizeof(headerPeek);
		if (good && headerPeek.IsValidPNGHeader()) {
			level.w = headerPeek.Width();
			level.h = headerPeek.Height();
			good = true;
		} else {
			ERROR_LOG(G3D, "Could not get PNG dimensions: %s (zip)", filename.c_str());
			good = false;
		}
		*pixelFormat = Draw::DataFormat::R8G8B8A8_UNORM;
	} else {
		ERROR_LOG(G3D, "Could not load texture replacement info: %s - unsupported format %s", filename.c_str(), magic.c_str());
	}


	// Already populated from cache. TODO: Move this above the first read, and take level.w/h from the cache.
	if (!levelData_->data[mipLevel].empty()) {
		vfs_->CloseFile(openFile);
		*pixelFormat = levelData_->fmt;
		return LoadLevelResult::DONE;
	}

	// Is this really the right place to do it?
	level.w = (level.w * desc_->w) / desc_->newW;
	level.h = (level.h * desc_->h) / desc_->newH;

	if (good && mipLevel != 0) {
		// Check that the mipmap size is correct.  Can't load mips of the wrong size.
		if (level.w != (levels_[0].w >> mipLevel) || level.h != (levels_[0].h >> mipLevel)) {
			WARN_LOG(G3D, "Replacement mipmap invalid: size=%dx%d, expected=%dx%d (level %d)",
				level.w, level.h, levels_[0].w >> mipLevel, levels_[0].h >> mipLevel, mipLevel);
			good = false;
		}
	}

	if (!good) {
		vfs_->CloseFile(openFile);
		return LoadLevelResult::LOAD_ERROR;
	}

	auto cleanup = [&] {
		vfs_->CloseFile(openFile);
	};

	vfs_->Rewind(openFile);

	level.fileRef = fileRef;

	if (imageType == ReplacedImageType::KTX2) {
		// Just slurp the whole file in one go and feed to the decoder.
		std::vector<uint8_t> buffer;
		buffer.resize(fileSize);
		buffer.resize(vfs_->Read(openFile, &buffer[0], buffer.size()));

		basist::ktx2_transcoder transcoder;
		if (!transcoder.init(buffer.data(), (int)buffer.size())) {
			WARN_LOG(G3D, "Error reading KTX file");
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}

		// Figure out the target format.
		basist::transcoder_texture_format transcoderFormat;
		if (transcoder.is_etc1s()) {
			// We only support opaque colors with this compression method.
			alphaStatus_ = ReplacedTextureAlpha::FULL;
			// Let's pick a suitable compatible format.
			if (desc_->formatSupport.bc123) {
				transcoderFormat = basist::transcoder_texture_format::cTFBC1;
				*pixelFormat = Draw::DataFormat::BC1_RGBA_UNORM_BLOCK;
			} else if (desc_->formatSupport.etc2) {
				transcoderFormat = basist::transcoder_texture_format::cTFETC1_RGB;
				*pixelFormat = Draw::DataFormat::ETC2_R8G8B8_UNORM_BLOCK;
			} else {
				// Transcode to RGBA8 instead as a fallback. A bit slow and takes a lot of memory, but better than nothing.
				WARN_LOG(G3D, "Replacement texture format not supported - transcoding to RGBA8888");
				transcoderFormat = basist::transcoder_texture_format::cTFRGBA32;
				*pixelFormat = Draw::DataFormat::R8G8B8A8_UNORM;
			}
		} else if (transcoder.is_uastc()) {
			// TODO: Try to recover some indication of alpha from the actual data blocks.
			alphaStatus_ = ReplacedTextureAlpha::UNKNOWN;
			// Let's pick a suitable compatible format.
			if (desc_->formatSupport.bc7) {
				transcoderFormat = basist::transcoder_texture_format::cTFBC7_RGBA;
				*pixelFormat = Draw::DataFormat::BC7_UNORM_BLOCK;
			} else if (desc_->formatSupport.astc) {
				transcoderFormat = basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
				*pixelFormat = Draw::DataFormat::ASTC_4x4_UNORM_BLOCK;
			} else {
				// Transcode to RGBA8 instead as a fallback. A bit slow and takes a lot of memory, but better than nothing.
				WARN_LOG(G3D, "Replacement texture format not supported - transcoding to RGBA8888");
				transcoderFormat = basist::transcoder_texture_format::cTFRGBA32;
				*pixelFormat = Draw::DataFormat::R8G8B8A8_UNORM;
			}
		} else {
			WARN_LOG(G3D, "PPSSPP currently only supports KTX for basis/UASTC textures. This may change in the future.");
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}

		int blockSize;
		bool bc = Draw::DataFormatIsBlockCompressed(*pixelFormat, &blockSize);
		_dbg_assert_(bc || *pixelFormat == Draw::DataFormat::R8G8B8A8_UNORM);

		levelData_->data.resize(numMips);

		basist::ktx2_transcoder_state transcodeState;  // Each thread needs one of these.

		transcoder.start_transcoding();
		for (int i = 0; i < numMips; i++) {
			std::vector<uint8_t> &out = levelData_->data[mipLevel + i];

			basist::ktx2_image_level_info levelInfo;
			bool result = transcoder.get_image_level_info(levelInfo, i, 0, 0);
			_dbg_assert_(result);

			size_t dataSizeBytes = levelInfo.m_total_blocks * blockSize;
			size_t outputSize = levelInfo.m_total_blocks;
			size_t outputPitch = levelInfo.m_num_blocks_x;
			// Support transcoded-to-RGBA8888 images too.
			if (!bc) {
				dataSizeBytes = levelInfo.m_orig_width * levelInfo.m_orig_height * 4;
				outputSize = levelInfo.m_orig_width * levelInfo.m_orig_height;
				outputPitch = levelInfo.m_orig_width;
			}
			levelData_->data[i].resize(dataSizeBytes);

			transcoder.transcode_image_level(i, 0, 0, &out[0], (uint32_t)outputSize, transcoderFormat, 0, (uint32_t)outputPitch, level.h, -1, -1, &transcodeState);
			level.w = levelInfo.m_orig_width;
			level.h = levelInfo.m_orig_height;
			if (i != 0)
				level.fileRef = nullptr;
			levels_.push_back(level);
		}
		transcoder.clear();
		cleanup();

		return LoadLevelResult::DONE;  // don't read more levels
	} else if (imageType == ReplacedImageType::DDS) {
		// TODO: Do better with alphaStatus, it's possible.
		alphaStatus_ = ReplacedTextureAlpha::UNKNOWN;

		DDSHeader header;
		DDSHeaderDXT10 header10{};
		vfs_->Read(openFile, &header, sizeof(header));
		if (ddsDX10) {
			vfs_->Read(openFile, &header10, sizeof(header10));
		}

		int blockSize = 0;
		bool bc = Draw::DataFormatIsBlockCompressed(*pixelFormat, &blockSize);
		_dbg_assert_(bc);

		levelData_->data.resize(numMips);

		// A DDS File can contain multiple mipmaps.
		for (int i = 0; i < numMips; i++) {
			std::vector<uint8_t> &out = levelData_->data[mipLevel + i];

			int bytesToRead = RoundUpTo4(level.w) * RoundUpTo4(level.h) * blockSize / 16;
			out.resize(bytesToRead);

			size_t read_bytes = vfs_->Read(openFile, &out[0], bytesToRead);
			if (read_bytes != bytesToRead) {
				WARN_LOG(G3D, "DDS: Expected %d bytes, got %d", bytesToRead, (int)read_bytes);
			}

			levels_.push_back(level);
			level.w = std::max(level.w / 2, 1);
			level.h = std::max(level.h / 2, 1);
			if (i != 0)
				level.fileRef = nullptr;  // We only provide a fileref on level 0 if we have mipmaps.
		}
		cleanup();
		return LoadLevelResult::DONE;  // don't read more levels
	} else if (imageType == ReplacedImageType::ZIM) {

		std::unique_ptr<uint8_t[]> zim(new uint8_t[fileSize]);
		if (!zim) {
			ERROR_LOG(G3D, "Failed to allocate memory for texture replacement");
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}

		if (vfs_->Read(openFile, &zim[0], fileSize) != fileSize) {
			ERROR_LOG(G3D, "Could not load texture replacement: %s - failed to read ZIM", filename.c_str());
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}

		int w, h, f;
		uint8_t *image;
		std::vector<uint8_t> &out = levelData_->data[mipLevel];
		// TODO: Zim files can actually hold mipmaps (although no tool has ever been made to create them :P)
		if (LoadZIMPtr(&zim[0], fileSize, &w, &h, &f, &image)) {
			if (w > level.w || h > level.h) {
				ERROR_LOG(G3D, "Texture replacement changed since header read: %s", filename.c_str());
				cleanup();
				return LoadLevelResult::LOAD_ERROR;
			}

			out.resize(level.w * level.h * 4);
			if (w == level.w) {
				memcpy(&out[0], image, level.w * 4 * level.h);
			} else {
				for (int y = 0; y < h; ++y) {
					memcpy(&out[level.w * 4 * y], image + w * 4 * y, w * 4);
				}
			}
			free(image);

			CheckAlphaResult res = CheckAlpha32Rect((u32 *)&out[0], level.w, w, h, 0xFF000000);
			if (res == CHECKALPHA_ANY || mipLevel == 0) {
				alphaStatus_ = ReplacedTextureAlpha(res);
			}
			levels_.push_back(level);
		} else {
			good = false;
		}

		cleanup();
		return LoadLevelResult::CONTINUE;

	} else if (imageType == ReplacedImageType::PNG) {
		png_image png = {};
		png.version = PNG_IMAGE_VERSION;

		std::string pngdata;
		pngdata.resize(fileSize);
		pngdata.resize(vfs_->Read(openFile, &pngdata[0], fileSize));
		if (!png_image_begin_read_from_memory(&png, &pngdata[0], pngdata.size())) {
			ERROR_LOG(G3D, "Could not load texture replacement info: %s - %s (zip)", filename.c_str(), png.message);
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}
		if (png.width > (uint32_t)level.w || png.height > (uint32_t)level.h) {
			ERROR_LOG(G3D, "Texture replacement changed since header read: %s", filename.c_str());
			cleanup();
			return LoadLevelResult::LOAD_ERROR;
		}

		bool checkedAlpha = false;
		if ((png.format & PNG_FORMAT_FLAG_ALPHA) == 0) {
			// Well, we know for sure it doesn't have alpha.
			if (mipLevel == 0) {
				alphaStatus_ = ReplacedTextureAlpha::FULL;
			}
			checkedAlpha = true;
		}
		png.format = PNG_FORMAT_RGBA;

		std::vector<uint8_t> &out = levelData_->data[mipLevel];
		out.resize(level.w * level.h * 4);
		if (!png_image_finish_read(&png, nullptr, &out[0], level.w * 4, nullptr)) {
			ERROR_LOG(G3D, "Could not load texture replacement: %s - %s", filename.c_str(), png.message);
			cleanup();
			out.resize(0);
			return LoadLevelResult::LOAD_ERROR;
		}
		png_image_free(&png);

		if (!checkedAlpha) {
			// This will only check the hashed bits.
			CheckAlphaResult res = CheckAlpha32Rect((u32 *)&out[0], level.w, png.width, png.height, 0xFF000000);
			if (res == CHECKALPHA_ANY || mipLevel == 0) {
				alphaStatus_ = ReplacedTextureAlpha(res);
			}
		}

		levels_.push_back(level);

		cleanup();
		return LoadLevelResult::CONTINUE;
	} else {
		WARN_LOG(G3D, "Don't know how to load this image type! %d", (int)imageType);
		cleanup();
	}
	return LoadLevelResult::LOAD_ERROR;
}

bool ReplacedTexture::CopyLevelTo(int level, void *out, int rowPitch) {
	_assert_msg_((size_t)level < levels_.size(), "Invalid miplevel");
	_assert_msg_(out != nullptr && rowPitch > 0, "Invalid out/pitch");

	if (State() != ReplacementState::ACTIVE) {
		WARN_LOG(G3D, "Init not done yet");
		return false;
	}

	// We probably could avoid this lock, but better to play it safe.
	std::lock_guard<std::mutex> guard(levelData_->lock);

	const ReplacedTextureLevel &info = levels_[level];
	const std::vector<uint8_t> &data = levelData_->data[level];

	if (data.empty()) {
		WARN_LOG(G3D, "Level %d is empty", level);
		return false;
	}

#define PARALLEL_COPY

	if (fmt == Draw::DataFormat::R8G8B8A8_UNORM) {
		if (rowPitch < info.w * 4) {
			ERROR_LOG(G3D, "Replacement rowPitch=%d, but w=%d (level=%d)", rowPitch, info.w * 4, level);
			return false;
		}

		_assert_msg_(data.size() == info.w * info.h * 4, "Data has wrong size");

		if (rowPitch == info.w * 4) {
#ifdef PARALLEL_COPY
			memcpy(out, data.data(), info.w * 4 * info.h);
#else
			ParallelMemcpy(&g_threadManager, out, &data[0], info.w * 4 * info.h);
#endif
		} else {
#ifdef PARALLEL_COPY
			for (int y = 0; y < info.h; ++y) {
				memcpy((uint8_t *)out + rowPitch * y, &data[0] + info.w * 4 * y, info.w * 4);
			}
#else
			const int MIN_LINES_PER_THREAD = 4;
			ParallelRangeLoop(&g_threadManager, [&](int l, int h) {
				for (int y = l; y < h; ++y) {
					memcpy((uint8_t *)out + rowPitch * y, &data[0] + info.w * 4 * y, info.w * 4);
				}
				}, 0, info.h, MIN_LINES_PER_THREAD);
#endif
		}
	} else {
#ifdef PARALLEL_COPY
		// TODO: Add sanity checks here for other formats?
		ParallelMemcpy(&g_threadManager, out, data.data(), data.size());
#else
		memcpy(out, data.data(), data.size());
#endif
	}

	return true;
}

const char *StateString(ReplacementState state) {
	switch (state) {
	case ReplacementState::UNINITIALIZED: return "UNINITIALIZED";
	case ReplacementState::POPULATED: return "PREPARED";
	case ReplacementState::PENDING: return "PENDING";
	case ReplacementState::NOT_FOUND: return "NOT_FOUND";
	case ReplacementState::ACTIVE: return "ACTIVE";
	case ReplacementState::CANCEL_INIT: return "CANCEL_INIT";
	default: return "N/A";
	}
}
