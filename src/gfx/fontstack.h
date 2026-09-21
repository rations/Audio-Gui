// FontStack -- this GUI's two typefaces, loaded with FreeType and wrapped as Cairo font faces.
//
// Ported from the rations-amp plug-in editor, which is where the ownership rule below was worked
// out and written down. Michroma is the title face and Roboto the body face, so the window reads
// as part of the same family.
//
// THE OWNERSHIP RULE, which is the whole reason this class exists rather than a pair of
// cairo_ft_font_face_create_for_ft_face() calls at the call site: the FT_Face, the bytes behind it
// and the FT_Library are owned by the cairo_font_face_t, through a destroy callback. Never call
// FT_Done_Face on them. fontstack.cpp explains what goes wrong if you do.
//
// If a face fails to load, the fallback is a generic Cairo toy face of the same weight, so text
// still renders (one warning on stderr, then silence).

#pragma once

#include <cairo/cairo.h>

#include <memory>
#include <string>

namespace audiogui
{

//------------------------------------------------------------------------
class FontStack
{
public:
    FontStack() = default;
    ~FontStack();

    FontStack(const FontStack &) = delete;
    FontStack &operator=(const FontStack &) = delete;

    // Load both faces from <resourceDir>/fonts. Returns false if either face fell back to a
    // system toy font, having already warned -- the caller may care (tools/uirender measures text
    // and must not measure a substituted face), but the window itself carries on regardless.
    bool load(const std::string &resourceDir);

    cairo_font_face_t *title() const
    {
        return mTitle;
    }
    cairo_font_face_t *body() const
    {
        return mBody;
    }

    // The underlying FT_Face handles, as void* so this header does not drag FreeType into every
    // translation unit that draws. Null when the face fell back to a cairo toy font.
    //
    // FOR THE OFFLINE AUDIT ONLY. tools/uirender asks these whether a character the panel draws
    // actually has a glyph, which is not a question Canvas can answer: a missing glyph measures
    // as zero width, so it sails through a does-it-fit check and then renders as nothing at all.
    // Nothing in the window itself should call these.
    void *titleFtFace() const
    {
        return mTitleFt;
    }
    void *bodyFtFace() const
    {
        return mBodyFt;
    }

private:
    // Returns a cairo face that OWNS the FT_Face, the file bytes behind it and a reference to the
    // FreeType library (see the comment on the destructor in fontstack.cpp). *outFt is set to the
    // FT_Face purely so load() can tell a real face from a toy fallback.
    cairo_font_face_t *loadFace(const std::string &path, bool bold, void **outFt);

    cairo_font_face_t *mTitle = nullptr;
    cairo_font_face_t *mBody = nullptr;

    // Observers, NOT owners: the FT_Face and its file bytes belong to the cairo font face above
    // and are freed by cairo, which may be long after this object dies. Never FT_Done_Face these.
    void *mTitleFt = nullptr;
    void *mBodyFt = nullptr;

    // FT_Library, held by shared_ptr because each face keeps a reference of its own: the library
    // must outlive every face opened from it, and cairo decides when those die.
    std::shared_ptr<void> mLibrary;
};

} // namespace audiogui
