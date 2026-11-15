#!/usr/bin/env python3
"""One-off M5 test update for tests/test_ai.cpp.

The M4 utility-AI tests verify M4 semantics, and the M5 spec requires
--morale off to reproduce the M4 utility results exactly — so every
utility-mode config in that file pins morale off. Assertions unchanged.
"""
PATH_ = "tests/test_ai.cpp"

with open(PATH_) as f:
    text = f.read()

n1 = text.count("config.ai_mode = sim::AiMode::Utility;  // the default, pinned for clarity")
text = text.replace(
    "config.ai_mode = sim::AiMode::Utility;  // the default, pinned for clarity",
    "config.ai_mode = sim::AiMode::Utility;  // the default, pinned for clarity\n"
    "  config.morale_enabled = false;  // M5: M4 semantics (morale off == M4 exactly)")

n2 = text.count("config.ai_mode = sim::AiMode::Utility;")
text = text.replace(
    "config.ai_mode = sim::AiMode::Utility;",
    "config.ai_mode = sim::AiMode::Utility;\n"
    "  config.morale_enabled = false;  // M5: M4 semantics")

n3 = text.count("base.ai_mode = sim::AiMode::Utility;")
text = text.replace(
    "base.ai_mode = sim::AiMode::Utility;",
    "base.ai_mode = sim::AiMode::Utility;\n"
    "  base.morale_enabled = false;  // M5: M4 semantics")

with open(PATH_, "w") as f:
    f.write(text)
print(f"utility_config: {n1}, inline configs: {n2}, base configs: {n3}")
