#pragma once

#include "ppsspp_config.h"

#include <map>
#include "Common/Render/Text/draw_text.h"

#if PPSSPP_PLATFORM(ANDROID)

#include <jni.h>

struct AndroidFontEntry {
	double size;
};

class TextDrawerAndroid : public TextDrawer {
public:
	TextDrawerAndroid(Draw::DrawContext *draw);
	~TextDrawerAndroid();

	bool IsReady() const override;
	uint32_t SetFont(const char *fontName, int size, int flags) override;
	void SetFont(uint32_t fontHandle) override;  // Shortcut once you've set the font once.
	void MeasureString(std::string_view str, float *w, float *h) override;
	void MeasureStringRect(std::string_view str, const Bounds &bounds, float *w, float *h, int align = ALIGN_TOPLEFT) override;
	bool DrawStringBitmap(std::vector<uint8_t> &bitmapData, TextStringEntry &entry, Draw::DataFormat texFormat, std::string_view str, int align, bool fullColor) override;
	// Use for housekeeping like throwing out old strings.
	void OncePerFrame() override;

protected:
	bool SupportsColorEmoji() const { return true; }

	void ClearCache() override;

private:
	// JNI functions
	jclass cls_textRenderer;
	jmethodID method_measureText;
	jmethodID method_renderText;

	std::map<uint32_t, AndroidFontEntry> fontMap_;
};

#endif
