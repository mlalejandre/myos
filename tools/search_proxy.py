#!/usr/bin/env python3
# search_proxy.py — Proxy HTTP de búsqueda web universal y totipotencial para SOMA.
from __future__ import annotations

import argparse
import html
import json
import math
import re
import sys
import threading
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_proxy_server = None

USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

STOP_WORDS = {
    "el", "la", "los", "las", "un", "una", "unos", "unas", "de", "del", "a", "al", "en",
    "con", "por", "para", "que", "se", "su", "sus", "y", "o", "u", "e", "es", "son",
    "fue", "era", "ha", "han", "este", "esta", "estos", "estas", "como", "cuando", "donde",
    "quien", "cual", "cuales", "busca", "buscar", "noticias", "noticia", "dime", "estan",
    "the", "of", "and", "in", "to", "for", "is", "on", "that", "by", "this", "with", "i", "you"
}


def clean_text(text: str) -> str:
    t = re.sub(r"<[^>]+>", " ", text)
    t = html.unescape(t)
    t = re.sub(r"\s+", " ", t)
    return t.strip()


def clean_query(q: str) -> str:
    return q.strip().strip("'\"`").strip()


def fetch_wikipedia_search(query: str, lang: str = "es") -> list[dict]:
    clean_q = clean_query(query)
    if not clean_q:
        return []
    url = (
        f"https://{lang}.wikipedia.org/w/api.php?action=query"
        f"&generator=search&gsrsearch={urllib.parse.quote(clean_q)}&gsrlimit=3"
        f"&prop=extracts&exintro=1&explaintext=1&exlimit=3&format=json"
    )
    req = urllib.request.Request(url, headers={"User-Agent": "SOMA-OS-Kernel/2.0 (admin@soma-os.local)"})
    results = []
    try:
        with urllib.request.urlopen(req, timeout=6) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
        pages = data.get("query", {}).get("pages", {})
        for _, pdata in pages.items():
            title = pdata.get("title", "")
            extract = clean_text(pdata.get("extract", ""))
            if title and extract:
                sentences = re.split(r'(?<=[.!?])\s+', extract)
                full_para = " ".join(sentences[:4]).strip()
                if full_para:
                    results.append({
                        "title": f"Wikipedia: {title}",
                        "snippet": full_para
                    })
    except Exception:
        pass
    return results


def fetch_bing_news_rss(query: str) -> list[dict]:
    clean_q = clean_query(query)
    if not clean_q:
        return []
    url = f"https://www.bing.com/news/search?q={urllib.parse.quote(clean_q)}&format=rss&setlang=es"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
    results = []
    try:
        with urllib.request.urlopen(req, timeout=6) as resp:
            xml_data = resp.read().decode("utf-8", errors="replace")
        root = ET.fromstring(xml_data)
        for item in root.findall(".//item")[:4]:
            title = clean_text(item.findtext("title") or "")
            desc = clean_text(item.findtext("description") or "")
            if title:
                snippet = f"{title}. {desc}" if desc and desc != title else title
                results.append({
                    "title": f"[Prensa] {title}",
                    "snippet": snippet
                })
    except Exception:
        pass
    return results


def fetch_ddg_lite(query: str) -> list[dict]:
    clean_q = clean_query(query)
    if not clean_q:
        return []
    url = "https://lite.duckduckgo.com/lite/"
    data = urllib.parse.urlencode({"q": clean_q, "kl": "es-es"}).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "User-Agent": USER_AGENT,
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept-Language": "es-ES,es;q=0.9,en;q=0.8",
        },
    )
    results = []
    try:
        with urllib.request.urlopen(req, timeout=6) as resp:
            content = resp.read().decode("utf-8", errors="replace")
        links = re.findall(r'<a class=["\']result-link["\'][^>]*>(.*?)</a>', content, re.DOTALL)
        snippets = re.findall(r'<td class=["\']result-snippet["\'][^>]*>(.*?)</td>', content, re.DOTALL)
        for i in range(min(len(links), len(snippets), 4)):
            t = clean_text(links[i])
            s = clean_text(snippets[i])
            if t and s:
                results.append({"title": t, "snippet": s})
    except Exception:
        pass
    return results


def perform_search(query: str) -> str:
    cleaned = clean_query(query)
    if not cleaned:
        return "Consulta vacía."

    raw_tokens = re.findall(r"[a-zA-Z0-9áéíóúÁÉÍÓÚñÑ]{3,}", cleaned.lower())
    query_tokens = [t for t in raw_tokens if t not in STOP_WORDS] or raw_tokens

    # Búsqueda concurrente agnóstica
    with ThreadPoolExecutor(max_workers=3) as executor:
        f_wiki = executor.submit(fetch_wikipedia_search, cleaned)
        f_bing = executor.submit(fetch_bing_news_rss, cleaned)
        f_ddg = executor.submit(fetch_ddg_lite, cleaned)

        all_res = f_wiki.result() + f_bing.result() + f_ddg.result()

    if not all_res:
        return f"Sin resultados web para: '{query}'."

    # Ponderación basada en densidad de términos de la consulta
    def score_item(item: dict) -> float:
        text = (item["title"] + " " + item["snippet"]).lower()
        score = 0.0
        for token in query_tokens:
            matches = len(re.findall(r'\b' + re.escape(token) + r'\b', text))
            score += matches * 2.0
            if token in item["title"].lower():
                score += 3.0
        return score

    all_res.sort(key=score_item, reverse=True)

    seen = set()
    dedup = []
    for item in all_res:
        norm = re.sub(r"[^a-zA-Z0-9]", "", item["title"][:40].lower())
        if norm and norm not in seen:
            seen.add(norm)
            dedup.append(item)

    formatted = []
    for i, it in enumerate(dedup[:4], 1):
        snip = it["snippet"]
        if len(snip) > 500:
            last_dot = snip[:500].rfind(".")
            if last_dot > 200:
                snip = snip[:last_dot + 1]
        formatted.append(f"[{i}] {it['title']}\n    {snip}")

    return "\n\n".join(formatted) if formatted else "Sin resultados relevantes."


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)
        q = (qs.get("q") or qs.get("query") or [""])[0].strip()

        if path in ("/health", "/"):
            self._send(200, b"ok\n", "text/plain; charset=utf-8")
            return
        if path not in ("/search", "/ddg"):
            self._send(404, b"not found\n", "text/plain")
            return
        if not q:
            self._send(400, b"missing q=\n", "text/plain")
            return

        text = perform_search(q) + "\n"
        self._send(200, text.encode("utf-8"), "text/plain; charset=utf-8")


def start_background(host: str = "0.0.0.0", port: int = 8095) -> bool:
    global _proxy_server
    try:
        httpd = ThreadingHTTPServer((host, port), Handler)
        t = threading.Thread(target=httpd.serve_forever, daemon=True, name="SearchProxyThread")
        t.start()
        _proxy_server = httpd
        return True
    except Exception as e:
        print(f"[SEARCH] AVISO: No se pudo iniciar proxy en {host}:{port} -> {e}")
        return False


def add_arguments(ap) -> None:
    ap.add_argument("--sin-busqueda", "--sin-proxy", action="store_true",
                    dest="sin_busqueda", help="no iniciar el proxy de busqueda web integrado")
    ap.add_argument("--proxy-port", type=int, default=8095,
                    help="puerto para el proxy de busqueda web (por defecto 8095)")


def ensure_proxy(args=None) -> None:
    port = getattr(args, "proxy_port", 8095) if args else 8095
    if getattr(args, "sin_busqueda", False):
        return

    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=0.8) as r:
            if r.status == 200:
                return
    except Exception:
        pass

    start_background("0.0.0.0", port)


def main():
    ap = argparse.ArgumentParser(description="Proxy de búsqueda agnóstico para SOMA")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8095)
    args = ap.parse_args()
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[search_proxy] Activo en http://{args.host}:{args.port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())