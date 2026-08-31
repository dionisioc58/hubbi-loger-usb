#!/usr/bin/env python3
"""Converte um log de captura com codigos ANSI em HTML autocontido.

Reproduz a aparencia do terminal do VS Code (tema Dark+), preservando as cores
que o ESP-IDF emite: verde para I, amarelo para W, vermelho para E.

Uso:
    python3 tools/log2html.py uart0_capture.log -o uart0_capture.html
    python3 tools/log2html.py uart0_capture.log            # -> uart0_capture.html
    cat captura.log | python3 tools/log2html.py - -o saida.html
"""
import argparse
import html
import re
import sys
from pathlib import Path

# Paleta do terminal do VS Code, tema Dark+.
NORMAL = {
    30: "#000000", 31: "#cd3131", 32: "#0dbc79", 33: "#e5e510",
    34: "#2472c8", 35: "#bc3fbc", 36: "#11a8cd", 37: "#e5e5e5",
}
BRIGHT = {
    90: "#666666", 91: "#f14c4c", 92: "#23d18b", 93: "#f5f543",
    94: "#3b8eea", 95: "#d670d6", 96: "#29b8db", 97: "#e5e5e5",
}
FOREGROUND = "#cccccc"
BACKGROUND = "#1e1e1e"

# Sequencias CSI: capturamos os parametros e a letra final.
CSI = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")


class Style:
    """Estado grafico corrente do terminal."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.fg = None
        self.bg = None
        self.bold = False
        self.underline = False

    def apply(self, params):
        # Um SGR sem parametros equivale a "0" (reset).
        codes = [int(p) if p else 0 for p in params.split(";")] or [0]
        i = 0
        while i < len(codes):
            c = codes[i]
            if c == 0:
                self.reset()
            elif c == 1:
                self.bold = True
            elif c == 22:
                self.bold = False
            elif c == 4:
                self.underline = True
            elif c == 24:
                self.underline = False
            elif c in NORMAL:
                self.fg = NORMAL[c]
            elif c in BRIGHT:
                self.fg = BRIGHT[c]
            elif 40 <= c <= 47:
                self.bg = NORMAL[c - 10]
            elif 100 <= c <= 107:
                self.bg = BRIGHT[c - 10]
            elif c == 39:
                self.fg = None
            elif c == 49:
                self.bg = None
            elif c in (38, 48):
                # Cor estendida: 5;<n> (256 cores) ou 2;<r>;<g>;<b>.
                target = "fg" if c == 38 else "bg"
                if i + 1 < len(codes) and codes[i + 1] == 5 and i + 2 < len(codes):
                    setattr(self, target, xterm256(codes[i + 2]))
                    i += 2
                elif i + 1 < len(codes) and codes[i + 1] == 2 and i + 4 < len(codes):
                    r, g, b = codes[i + 2], codes[i + 3], codes[i + 4]
                    setattr(self, target, f"#{r:02x}{g:02x}{b:02x}")
                    i += 4
            i += 1

    def css(self):
        parts = []
        if self.fg:
            parts.append(f"color:{self.fg}")
        if self.bg:
            parts.append(f"background:{self.bg}")
        if self.bold:
            parts.append("font-weight:700")
        if self.underline:
            parts.append("text-decoration:underline")
        return ";".join(parts)


def xterm256(n):
    """Converte um indice da paleta de 256 cores em hexadecimal."""
    if n < 8:
        return NORMAL[30 + n]
    if n < 16:
        return BRIGHT[90 + (n - 8)]
    if n < 232:
        n -= 16
        levels = [0, 95, 135, 175, 215, 255]
        r, g, b = levels[n // 36], levels[(n // 6) % 6], levels[n % 6]
        return f"#{r:02x}{g:02x}{b:02x}"
    v = 8 + (n - 232) * 10
    return f"#{v:02x}{v:02x}{v:02x}"


def convert(text):
    """Traduz o texto com ANSI em fragmentos HTML."""
    style = Style()
    out = []
    open_span = False

    def close():
        nonlocal open_span
        if open_span:
            out.append("</span>")
            open_span = False

    pos = 0
    for m in CSI.finditer(text):
        chunk = text[pos:m.start()]
        if chunk:
            out.append(html.escape(chunk))
        # Sequencias que nao sao SGR (cursor, limpeza de tela) sao descartadas:
        # nao ha equivalente sensato numa pagina estatica.
        if m.group(2) == "m":
            close()
            style.apply(m.group(1))
            css = style.css()
            if css:
                out.append(f'<span style="{css}">')
                open_span = True
        pos = m.end()

    tail = text[pos:]
    if tail:
        out.append(html.escape(tail))
    close()
    return "".join(out)


TEMPLATE = """<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  html {{ background: {bg}; }}
  body {{
    margin: 0; padding: 16px;
    background: {bg}; color: {fg};
    font-family: "SF Mono", Menlo, Consolas, "DejaVu Sans Mono", monospace;
    font-size: 12px; line-height: 1.4;
  }}
  pre {{ margin: 0; white-space: pre-wrap; word-break: break-word; }}
  .meta {{
    color: #808080; font-size: 11px;
    border-bottom: 1px solid #333; padding-bottom: 8px; margin-bottom: 12px;
  }}
</style>
</head>
<body>
<div class="meta">{title} &middot; {lines} linhas &middot; {size} bytes</div>
<pre>{body}</pre>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("entrada", help="arquivo .log capturado (ou - para stdin)")
    ap.add_argument("-o", "--saida", help="arquivo .html de saida")
    args = ap.parse_args()

    if args.entrada == "-":
        raw = sys.stdin.buffer.read()
        nome = "captura"
    else:
        caminho = Path(args.entrada)
        raw = caminho.read_bytes()
        nome = caminho.name

    # A captura e bruta e pode conter bytes truncados no meio de um caractere.
    texto = raw.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")

    doc = TEMPLATE.format(
        title=html.escape(nome),
        bg=BACKGROUND,
        fg=FOREGROUND,
        lines=texto.count("\n") + 1,
        size=len(raw),
        body=convert(texto),
    )

    destino = Path(args.saida) if args.saida else Path(args.entrada).with_suffix(".html")
    destino.write_text(doc, encoding="utf-8")
    print(f"{destino}  ({len(doc)} bytes)")


if __name__ == "__main__":
    main()
