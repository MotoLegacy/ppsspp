#pragma once

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)

#include <map>
#include "Common/Render/Text/draw_text.h"

struct TextDrawerContext;
// Internal struct but all details in .cpp file (pimpl to avoid pulling in excessive headers here)
class TextDrawerFontContext;

class TextDrawerCocoa : public TextDrawer {
public:
	TextDrawerCocoa(Draw::DrawContext *draw);
	~TextDrawerCocoa();

	uint32_t SetFont(const char *fontName, int size, int flags) override;
	void SetFont(uint32_t fontHandle) override;  // Shortcut once you've set the font once.
	void MeasureString(std::string_view str, float *w, float *h) override;
	void MeasureStringRect(std::string_view str, const Bounds &bounds, float *w, float *h, int align = ALIGN_TOPLEFT) override;
	bool DrawStringBitmap(std::vector<uint8_t> &bitmapData, TextStringEntry &entry, Draw::DataFormat texFormat, std::string_view str, int align, bool fullColor) override;
	// Use for housekeeping like throwing out old strings.
	void OncePerFrame() override;

protected:
	bool SupportsColorEmoji() const override { return true; }

	void ClearCache() override;
	void RecreateFonts();  // On DPI change

	TextDrawerContext *ctx_;
	std::map<uint32_t, std::unique_ptr<TextDrawerFontContext>> fontMap_;
};

#endif
