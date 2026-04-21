#pragma once
#include <cstdint>
#include <ctime>

struct ReadItLaterArticle {
  uint64_t id = 0;
  char title[128] = {0};
  char domain[64] = {0};
  char author[64] = {0};
  uint32_t word_count = 0;
  uint32_t progress_pct = 0;
  bool starred = false;
  time_t saved_at = 0;
};
