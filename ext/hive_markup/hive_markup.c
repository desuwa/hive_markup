#define RSTRING_NOT_MODIFIED

#include <ruby/ruby.h>
#include <ruby/encoding.h>
#include <ctype.h>

#include "houdini.h"

#define MIN_BUF_SIZE 128
#define MAX_BUF_SIZE 2 * 1024 * 1024
#define buf_puts(buf, str) buf_append(buf, str, sizeof str - 1)

static VALUE rb_mHive;
static VALUE rb_cMarkup;

typedef struct {
  uint8_t *data;
  size_t size;
  size_t total;
  uint8_t fragile;
} buffer;

static size_t parse_text(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_em(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_maybequote(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_quote(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_quotelink(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_linebreak(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_codeblock(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_aablock(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_spoiler(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_escape(const uint8_t *text, size_t start, size_t size, buffer *out_buf);
static size_t parse_autolink(const uint8_t *text, size_t start, size_t size, buffer *out_buf);

typedef size_t (*trigger_func)(const uint8_t *text, size_t start, size_t size, buffer *out_buf);

static char trigger_map[256];

enum trigger_keys {
  HIVE_NULL = 0,
  HIVE_EM,
  HIVE_QUOTE,
  HIVE_LINEBREAK,
  HIVE_CODE,
  HIVE_SPOILER,
  HIVE_ESCAPE,
  HIVE_AA,
  HIVE_AUTOLINK
};

static trigger_func trigger_funcs[] = {
  0,
  parse_em,
  parse_maybequote,
  parse_linebreak,
  parse_codeblock,
  parse_spoiler,
  parse_escape,
  parse_aablock,
  parse_autolink
};

static void init_triggers() {
  trigger_map['*'] = HIVE_EM;
  trigger_map['>'] = HIVE_QUOTE;
  trigger_map['\n'] = HIVE_LINEBREAK;
  trigger_map['`'] = HIVE_CODE;
  trigger_map['$'] = HIVE_SPOILER;
  trigger_map['\\'] = HIVE_ESCAPE;
  trigger_map['~'] = HIVE_AA;
  trigger_map['/'] = HIVE_AUTOLINK;
}

static buffer * buf_new() {
  buffer *buf;
  
  buf = malloc(sizeof (buffer));
  
  if (buf) {
    buf->data = 0;
    buf->size = 0;
    buf->total = 0;
    buf->fragile = 0;
  }
  
  return buf;
}

static int buf_expand(buffer *buf, size_t new_size) {
  void *new_ptr;
  
  new_size = MIN_BUF_SIZE + new_size + (new_size >> 1);
  
  if (new_size > MAX_BUF_SIZE) {
    return 0;
  }
  
  new_ptr = realloc(buf->data, new_size);
  
  if (new_ptr) {
    buf->data = new_ptr;
    buf->total = new_size;
    return 1;
  }
  
  return 0;
}

static void buf_append(buffer *buf, const void *str, size_t size) {
  if (buf->size + size > buf->total && !buf_expand(buf, buf->size + size)) {
    return;
  }
  
  memcpy(buf->data + buf->size, str, size);
  buf->size += size;
}

static void buf_putc(buffer *buf, uint8_t c) {
  if (buf->size >= buf->total && !buf_expand(buf, buf->size + 1)) {
    return;
  }
  
  buf->data[buf->size] = c;
  ++buf->size;
}

static void buf_free(buffer *buf) {
  free(buf->data);
  free(buf);
}

static inline int is_space(uint8_t c) {
	return c == ' ' || c == '\n';
}

static inline int is_alnum(uint8_t c) {
  return isalnum(c) && c < 0x7b;
}

static inline int is_alpha(uint8_t c) {
  return isalpha(c) && c < 0x7b;
}

static inline int is_digit(uint8_t c) {
  return c >= '0' && c <= '9';
}

static inline int is_xdigit(uint8_t c) {
  return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static void preprocess_text(const uint8_t *text, size_t size, buffer *out_buf) {
  size_t end = 0;
  
  while (end < size) {
    size_t from = end;
    
    while (end < size && text[end] > 31 && text[end] != 127) {
      ++end;
    }
    
    if (end > from) {
      buf_append(out_buf, text + from, end - from);
    }
    
    if (end >= size) {
      break;
    }
    
    if (text[end] == '\n') {
      buf_putc(out_buf, '\n');
    }
    else if (text[end] == '\t') {
      buf_putc(out_buf, ' ');
      buf_putc(out_buf, ' ');
    }
    
    ++end;
  }
}

static void escape_html_char(uint8_t c, buffer *out_buf) {
  uint8_t esc;
  
  if ((esc = HTML_ESCAPE_TABLE[c]) != 0) {
    buf_append(out_buf, HTML_ESCAPES[esc], HTML_ESCAPES_LEN[esc]);
  }
  else {
    buf_putc(out_buf, c);
  }
}

static size_t escape_html(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  uint8_t esc = 0;
  size_t end = start;
  
  while (end < size) {
    size_t from = end;
    
    while (end < size && (esc = HTML_ESCAPE_TABLE[text[end]]) == 0) {
      ++end;
    }
    
    if (end > from) {
      buf_append(out_buf, text + from, end - from);
    }
    
    if (end >= size) {
      break;
    }
    
    buf_append(out_buf, HTML_ESCAPES[esc], HTML_ESCAPES_LEN[esc]);
    
    ++end;
  }
  
  return end - start;
}

static size_t parse_aablock(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (out_buf->fragile) {
    return 0;
  }
  
  if (start > 0 && text[start - 1] != '\n') {
    return 0;
  }
  
  if (start + 2 >= size || text[start + 1] != '~' || text[start + 2] != '~') {
    return 0;
  }
  
  size_t end = start + 3;
  
  while (end < size && text[end] == '\n') {
    ++end;
  }
  
  if (end >= size) {
    return 0;
  }
  
  size_t i, block_start = end;
  
  for (i = 0; end < size; ++end) {
    if (text[end] != '~' || text[end - 1] == '\\') {
      i = 0;
    }
    else {
      ++i;
    }
    if (i == 3 && text[end - 3] == '\n' && (end + 1 >= size || text[end + 1] == '\n')) {
      break;
    }
  }
  
  if (i < 3) {
    return 0;
  }
  
  size_t block_end = end - 2;
  
  while (block_end > block_start && text[block_end - 1] == '\n') {
    block_end--;
  }
  
  if (block_end <= block_start) {
    return 0;
  }
  
  buf_puts(out_buf, "<pre class=\"aa\">");
  
  escape_html(text, block_start, block_end, out_buf);
  
  buf_puts(out_buf, "</pre>");
  
  if (end + 1 < size) {
    ++end;
  }
  
  return end - start + 1;
}

static size_t parse_codeblock(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (out_buf->fragile) {
    return 0;
  }
  
  if (start > 0 && text[start - 1] != '\n') {
    return 0;
  }
  
  if (start + 2 >= size || text[start + 1] != '`' || text[start + 2] != '`') {
    return 0;
  }
  
  size_t end = start + 3;
  
  while (end < size && text[end] == '\n') {
    ++end;
  }
  
  if (end >= size) {
    return 0;
  }
  
  size_t i, block_start = end;
  
  for (i = 0; end < size; ++end) {
    if (text[end] != '`' || text[end - 1] == '\\') {
      i = 0;
    }
    else {
      ++i;
    }
    if (i == 3 && text[end - 3] == '\n' && (end + 1 >= size || text[end + 1] == '\n')) {
      break;
    }
  }
  
  if (i < 3) {
    return 0;
  }
  
  size_t block_end = end - 2;
  
  while (block_end > block_start && text[block_end - 1] == '\n') {
    block_end--;
  }
  
  if (block_end <= block_start) {
    return 0;
  }
  
  buf_puts(out_buf, "<pre class=\"code\"><code class=\"prettyprint\">");
  
  escape_html(text, block_start, block_end, out_buf);
  
  buf_puts(out_buf, "</code></pre>");
  
  if (end + 1 < size) {
    ++end;
  }
  
  return end - start + 1;
}

static size_t parse_spoiler(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (start > 0 && is_alnum(text[start - 1])) {
    return 0;
  }
  
  size_t end = start + 1;
  
  while (end < size && text[end] == '$') {
    ++end;
  }
  
  if (end - start != 2) {
    buf_append(out_buf, text + start, end - start); // $
    return end - start;
  }
  
  while (end < size && text[end] == '\n') {
    ++end;
  }
  
  if (end >= size) {
    return 0;
  }
  
  size_t block_start = end;
  
  while (end < size) {
    if (text[end] == '$'
      && end + 1 < size
      && text[end + 1] == '$'
      && (end + 1 >= size || !is_alnum(text[end + 1]))
      ) {
      break;
    }
    ++end;
  }
  
  if (end >= size) {
    return 0;
  }
  
  size_t block_end = end;
  
  while (block_end > block_start && text[block_end - 1] == '\n') {
    block_end--;
  }
  
  if (block_end <= block_start) {
    return 0;
  }
  
  out_buf->fragile = 1;
  
  buf_puts(out_buf, "<span class=\"s\">");
  
  parse_text(text, block_start, block_end, out_buf);
  
  buf_puts(out_buf, "</span>");
  
  out_buf->fragile = 0;
  
  return end - start + 2;
}

static size_t parse_em(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (start > 0 && is_alnum(text[start - 1])) {
    return 0;
  }
  
  size_t end = start + 1;
  
  while (end < size && text[end] == '*') {
    ++end;
  }
  
  if (end - start > 1) {
    buf_append(out_buf, text + start, end - start); // *
    return end - start;
  }
  
  if (end < size && is_space(text[end])) {
    return 0;
  }
  
  while (end < size && text[end] != '\n') {
    if (text[end] == '*' && !is_space(text[end - 1])
      && (end + 1 >= size || !is_alnum(text[end + 1]))
      && text[end - 1] != '\\') {
      break;
    }
    ++end;
  }
  
  if (end >= size || text[end] == '\n') {
    return 0;
  }
  
  if (is_space(text[end - 1])) {
    return 0;
  }
  
  ++start;
  
  buf_puts(out_buf, "<em>");
  
  parse_text(text, start, end, out_buf);
  
  buf_puts(out_buf, "</em>");
  
  return end - start + 2;
}

static size_t parse_escape(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (start + 1 < size) {
    uint8_t c = text[start + 1];
    if (c == '*' || c == '`' || c == '$' || c == '~') {
      escape_html_char(c, out_buf);
      return 2;
    }
  }
  return 0;
}

static size_t parse_autolink(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  static const char *autolink_scheme = "http";
  static const char *punct = ":;!?,.'\"&";
  
  if (start < 5 || start + 2 >= size || text[start - 1] != ':' || text[start + 1] != '/') {
    return 0;
  }
  
  size_t block_start, scheme_len;
  
  if (text[start - 2] != 's') {
    scheme_len = 5;
  }
  else {
    scheme_len = 6;
  }
  
  if (scheme_len > start) {
    return 0;
  }
  
  block_start = start - scheme_len;
  
  if (memcmp(&text[block_start], autolink_scheme, 4) != 0) {
    return 0;
  }
  
  size_t block_end = start + 2;
  
  while (block_end < size && !is_space(text[block_end])) {
    ++block_end;
  }
  
  while (block_end > start) {
    if (strchr(punct, text[block_end - 1]) == 0) {
      break;
    }
    block_end--;
  }
  
  size_t i = block_end;
  
  while (i > start && text[i - 1] == ')') {
    i--;
  }
  
  if (i < block_end) {
    block_end = i;
    while (i > start) {
      if (text[i] == '(') {
        ++block_end;
        break;
      }
      i--;
    }
  }
  
  out_buf->size -= scheme_len;
  
  buf_puts(out_buf, "<a href=\"");
  escape_html(text, block_start, block_end, out_buf);
  buf_puts(out_buf, "\">");
  escape_html(text, block_start, block_end, out_buf);
  buf_puts(out_buf, "</a>");
  
  return block_end - start;
}

static size_t parse_linebreak(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  size_t count = 1;
  
  buf_puts(out_buf, "<br>");
  
  ++start;
  
  while (start < size && text[start] == '\n') {
    ++start;
    ++count;
  }
  
  if (count > 1) {
    buf_puts(out_buf, "<br>");
  }
  
  return count;
}

static size_t parse_maybequote(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  size_t count;
  
  if (count = parse_quotelink(text, start, size, out_buf)) {
    return count;
  }
  
  return parse_quote(text, start, size, out_buf);
}

static size_t parse_quotelink(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (start > 0 && is_alnum(text[start - 1])) {
    return 0;
  }
  
  ++start;
  
  if (start >= size || text[start] != '>') {
    return 0;
  }
  
  ++start;
  
  size_t end = start;
  size_t count = end - start;
  
  while (end < size && is_digit(text[end]) && count < 10) {
    ++end;
    ++count;
  }
  
  if (end == start || (end < size && is_alpha(text[end]))) {
    return 0;
  }
  
  size_t i;
  uint8_t num[count];
  
  for (i = 0; i < count; ++i) {
    num[i] = text[start + i];
  }
  
  buf_puts(out_buf, "<a class=\"ql\" href=\"#");
  buf_append(out_buf, &num, count);
  buf_puts(out_buf, "\">&gt;&gt;");
  buf_append(out_buf, &num, count);
  buf_puts(out_buf, "</a>");
  
  return count + 2;
}

static size_t parse_quote(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  if (start > 0 && text[start - 1] != '\n') {
    return 0;
  }
  
  size_t end = start;
  
  while (end < size && text[end] != '\n' ) {
    ++end;
  }
  
  size_t block_start = start + 1;
  
  while (block_start + 2 < size && text[block_start + 2] == '>') {
    ++block_start;
  }
  
  size_t count = block_start - start;
  
  buf_puts(out_buf, "<span class=\"q\">&gt;");
  
  while (count > 1) {
    buf_puts(out_buf, "&gt;");
    count--;
  }
  
  parse_text(text, block_start, end, out_buf);
  
  buf_puts(out_buf, "</span>");
  
  return end - start;
}

static size_t parse_text(const uint8_t *text, size_t start, size_t size, buffer *out_buf) {
  uint8_t action = 0;
  size_t from, count, end;
  
  end = start;
  
  while (end < size) {
    from = end;
    
    while (end < size && (action = trigger_map[text[end]]) == 0) {
      ++end;
    }
    
    if (end > from) {
      escape_html(text, from, end, out_buf);
    }
    
    if (end >= size) {
      break;
    }
    
    if ((count = trigger_funcs[action](text, end, size, out_buf)) != 0) {
      end += count;
    }
    else {
      escape_html(text, end, end + 1, out_buf);
      ++end;
    }
  }
  
  return end - start;
}

static VALUE rb_hive_markup_render(VALUE self, VALUE text) {
  Check_Type(text, T_STRING);
  
  size_t size;
  size = RSTRING_LEN(text);
  
  buffer *pre_buf = buf_new();
  
  if (!pre_buf) {
    return Qnil;
  }
  
  buffer *out_buf = buf_new();
  
  if (!out_buf) {
    return Qnil;
  }
  
  buf_expand(pre_buf, size);
  buf_expand(out_buf, size);
  
  preprocess_text((const uint8_t *)StringValuePtr(text), size, pre_buf);
  
  parse_text(pre_buf->data, 0, pre_buf->size, out_buf);
  
  text = rb_enc_str_new(
    (const char *)out_buf->data,
    out_buf->size,
    rb_enc_get(text)
  );
  
  buf_free(pre_buf);
  buf_free(out_buf);
  
  return text;
}

void Init_hive_markup() {
  init_triggers();
  
  rb_mHive = rb_define_module("Hive");
  rb_cMarkup = rb_define_class_under(rb_mHive, "Markup", rb_cObject);
  
  rb_define_singleton_method(rb_cMarkup, "render", rb_hive_markup_render, 1);
}
