# QA report — Nudge paper2ppt

- PPTX: `/Users/bytedance/PCA/output/nudge-paper2ppt/final_presentation_cn.pptx`
- Slide count: 15
- Embedded media files: 8
- Slides with speaker notes: 15
- Figure assets inserted: Fig.1, Fig.2, Table 3, Fig.4, Fig.5, Table 6, Fig.7, Table 8.
- Self-review: no high-severity unsupported quantitative claims; dense source visuals are either given large slide area or paired with concise interpretation.
- Shape bounds check: pass
- Text-density check: pass
- Design-rhythm check: varied cover, conceptual, workflow, figure-dominant, comparison, and discussion slides; avoided repeated card-only template.
- Rendered preview: not run; no reliable headless PPT renderer was used. Verification used python-pptx reopen, media count, shape bounds, contact sheet inspection, and text-density audit.
- Known limitation: figures are cropped from the PDF rather than original vector source files, so very small table text may still be easier to read in slideshow/fullscreen mode.
