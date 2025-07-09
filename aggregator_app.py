# -*- coding: utf-8 -*-

# --- Windows 7 相容性說明 ---
# 1. Python 版本: 此腳本建議使用 Python 3.8.x 版本運行，因為這是官方支援 Windows 7 的最後一個主要 Python 版本。
# 2. 安裝依賴庫: 為了確保在 Windows 7 上能成功發起 HTTPS 網路請求 (因其內建安全憑證可能過舊)，
#    強烈建議安裝 'certifi' 庫。請使用以下指令安裝所有必要的函式庫：
#    pip install requests pysocks certifi futures
# -----------------------------------

import tkinter as tk
from tkinter import scrolledtext, messagebox, ttk, filedialog
import base64
import time
import threading
import requests
import sys
import certifi
import json
import re
import traceback
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
import queue

# 為了支援SOCKS代理，需要安裝 PySocks
try:
    import socks
except ImportError:
    class FakeSocks:
        def set_default_proxy(self, *args, **kwargs):
            pass
    sys.modules['socks'] = FakeSocks()


# --- 通用右鍵菜單功能 ---
class TextWidgetContextMenu:
    def __init__(self, widget):
        self.widget = widget
        self.menu = tk.Menu(widget, tearoff=0)
        self.menu.add_command(label="剪下", command=self.cut)
        self.menu.add_command(label="複製", command=self.copy)
        self.menu.add_command(label="貼上", command=self.paste)
        self.menu.add_separator()
        self.menu.add_command(label="全選", command=self.select_all)
        widget.bind("<Button-3>", self.show_menu)

    def show_menu(self, event):
        try:
            if self.widget.selection_get():
                self.menu.entryconfig("剪下", state=tk.NORMAL); self.menu.entryconfig("複製", state=tk.NORMAL)
            else:
                self.menu.entryconfig("剪下", state=tk.DISABLED); self.menu.entryconfig("複製", state=tk.DISABLED)
        except tk.TclError:
            self.menu.entryconfig("剪下", state=tk.DISABLED); self.menu.entryconfig("複製", state=tk.DISABLED)
        try:
            if self.widget.clipboard_get(): self.menu.entryconfig("貼上", state=tk.NORMAL)
        except tk.TclError:
            self.menu.entryconfig("貼上", state=tk.DISABLED)
        self.menu.tk_popup(event.x_root, event.y_root)

    def cut(self): self.widget.event_generate("<<Cut>>")
    def copy(self): self.widget.event_generate("<<Copy>>")
    def paste(self): self.widget.event_generate("<<Paste>>")
    def select_all(self):
        if isinstance(self.widget, (ttk.Entry, tk.Entry)): self.widget.select_range(0, tk.END)
        elif isinstance(self.widget, (tk.Text, scrolledtext.ScrolledText)): self.widget.tag_add("sel", "1.0", "end")

# --- 帶佔位符的輸入框 ---
class PlaceholderEntry(ttk.Entry):
    def __init__(self, container, placeholder, *args, **kwargs):
        super().__init__(container, *args, **kwargs)
        self.placeholder = placeholder
        self.placeholder_color = 'grey'
        self.default_fg_color = self['foreground']
        self.is_placeholder = True

        self.bind("<FocusIn>", self._focus_in)
        self.bind("<FocusOut>", self._focus_out)

        self._put_placeholder()

    def _put_placeholder(self):
        self.delete(0, tk.END)
        self.insert(0, self.placeholder)
        self['foreground'] = self.placeholder_color
        self.is_placeholder = True

    def _focus_in(self, *args):
        if self.is_placeholder:
            self.delete('0', 'end')
            self['foreground'] = self.default_fg_color
            self.is_placeholder = False

    def _focus_out(self, *args):
        if not self.get():
            self._put_placeholder()
    
    def get_real_text(self):
        if self.is_placeholder:
            return ""
        return super().get()

# --- 從 nexavor/aggregator 專案移植的輔助函式 ---
class NexavorUtils:
    USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"
    DEFAULT_HTTP_HEADERS = {"User-Agent": USER_AGENT, "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"}
    NODE_PROTOCOLS = ("vmess://", "ss://", "ssr://", "trojan://", "vless://", "tuic://", "hysteria://")
    PROTOCOL_REGEX = r"(?:vmess|trojan|ss|ssr|vless|tuic|hysteria)://[a-zA-Z0-9:.?+=@%&#_\-/]{10,}"
    
    @staticmethod
    def http_get(url, headers=None, params=None, retry=3, proxy=None, timeout=15):
        if not (url.startswith('http://') or url.startswith('https://')): return ""
        if retry <= 0: return ""
        headers = headers if headers else NexavorUtils.DEFAULT_HTTP_HEADERS
        proxies = {'http': proxy, 'https': proxy} if proxy else None
        try:
            response = requests.get(url, headers=headers, params=params, proxies=proxies, timeout=timeout, verify=certifi.where())
            response.raise_for_status()
            return response.text
        except Exception:
            time.sleep(1)
            return NexavorUtils.http_get(url, headers, params, retry - 1, proxy, timeout)

# --- 整合後的並發搜索核心 ---
class DynamicSubscriptionFinder:
    def __init__(self, gui_queue, proxy_address=None, stop_event=None, lock=None):
        self.gui_queue = gui_queue
        self.proxy_address = proxy_address
        self.utils = NexavorUtils()
        self.stop_event = stop_event or threading.Event()
        self.lock = lock or threading.Lock()

    def _log(self, message):
        self.gui_queue.put(('log', message))

    def search_github_for_keyword(self, token, search_query, pages=2):
        if self.stop_event.is_set(): return []
        self._log(f"[並發搜索] 開始處理關鍵字: '{search_query}'...")
        links = set()
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28", "User-Agent": self.utils.USER_AGENT}
        if token: headers["Authorization"] = f"Bearer {token}"
        
        for page in range(1, pages + 1):
            if self.stop_event.is_set(): self._log(f"[並發搜索] 關鍵字 '{search_query}' 的任務已中止。"); break
            
            params = {'q': search_query, 'sort': 'indexed', 'order': 'desc', 'per_page': 100, 'page': page}
            api_url = "https://api.github.com/search/code"
            try:
                content = self.utils.http_get(api_url, params=params, headers=headers, proxy=self.proxy_address)
                if not content: continue
                data = json.loads(content)
                items = data.get("items", [])
                if not items: break
                
                for item in items:
                    if self.stop_event.is_set(): break
                    html_url = item.get("html_url")
                    if html_url: links.add(html_url.replace("github.com", "raw.githubusercontent.com").replace("/blob/", "/"))
                time.sleep(2)
            except Exception as e:
                self._log(f"[並發搜索] 處理關鍵字 '{search_query}' 第 {page} 頁時出錯: {e}"); break
        
        self._log(f"[並發搜索] 關鍵字 '{search_query}' 完成，找到 {len(links)} 個潛在連結。")
        return list(links)

    def fetch_and_extract_from_url(self, file_url):
        if self.stop_event.is_set(): return []
        
        content = self.utils.http_get(file_url, proxy=self.proxy_address)
        if not content: return []

        subscriptions = self.extract_subscriptions_from_content(content)
        if subscriptions:
            with self.lock:
                self._log(f"  => 從 {file_url} 找到 {len(subscriptions)} 個連結。")
        return subscriptions

    def extract_subscriptions_from_content(self, content):
        if not content: return []
        cleaned_links = set()
        sub_regex = r"https?://(?:[a-zA-Z0-9\u4e00-\u9fa5\-]+\.)+[a-zA-Z0-9\u4e00-\u9fa5\-]+(?:(?:(?:/index.php)?/api/v1/client/subscribe\?token=[a-zA-Z0-9]{16,32})|(?:/link/[a-zA-Z0-9]+\?(?:sub|mu|clash)=\d)|(?:/(?:s|sub)/[a-zA-Z0-9]{32}))"
        extra_regex = r"https?://(?:[a-zA-Z0-9\u4e00-\u9fa5\-]+\.)+[a-zA-Z0-9\u4e00-\u9fa5\-]+/sub\?(?:\S+)?target=\S+"
        
        cleaned_links.update(re.findall(sub_regex, content, re.I))
        cleaned_links.update(re.findall(extra_regex, content, re.I))
        cleaned_links.update(re.findall(self.utils.PROTOCOL_REGEX, content, re.I))
        return [link.strip() for link in cleaned_links]

    def find(self, executor, github_token, queries, pages):
        potential_files = set()
        self._log(f"=== 第一階段: 開始並發搜索 {len(queries)} 個關鍵字 ===")
        if not github_token: 
            self._log("[GitHub搜索] 嚴重警告：未提供任何有效 GitHub Token，搜索請求將極大概率失敗！")
        
        future_to_query = {executor.submit(self.search_github_for_keyword, github_token, f'"{query}" in:file', pages): query for query in queries}
        for future in as_completed(future_to_query):
            if self.stop_event.is_set(): break
            try: potential_files.update(future.result())
            except Exception as e: self._log(f"一個關鍵字搜索任務失敗: {e}")
        
        if self.stop_event.is_set(): return
        
        self._log(f"\n=== 第二階段: 從 {len(potential_files)} 個文件中並發提取訂閱連結... ===")
        if not potential_files: self._log("未找到任何可能的文件。"); return
            
        future_to_url = {executor.submit(self.fetch_and_extract_from_url, url): url for url in potential_files}
        for i, future in enumerate(as_completed(future_to_url)):
            if self.stop_event.is_set(): break
            if (i + 1) % 20 == 0 or (i + 1) == len(potential_files):
                self._log(f"提取進度: {i+1}/{len(potential_files)}")
            try:
                subscriptions = future.result()
                if subscriptions: self.gui_queue.put(('found_links', subscriptions))
            except Exception as e: self._log(f"一個文件提取任務失敗: {e}")

# --- 後端處理核心 ---
class RealProxyAggregator:
    def __init__(self, gui_queue):
        self.gui_queue = gui_queue

    def _log(self, msg): self.gui_queue.put(('log', msg))

    def fetch_and_parse_url(self, url, proxy_address=None):
        self._log(f"正在下載訂閱: {url}")
        try:
            raw_content = NexavorUtils.http_get(url, proxy=proxy_address)
            if not raw_content: raise ValueError("下載內容為空")
            
            # --- 內容驗證核心邏輯 ---
            # 1. 優先嘗試 Base64 解碼
            try:
                decoded_content = base64.b64decode(raw_content.strip()).decode('utf-8')
                self._log(f"  -> {url} 解碼為 Base64 成功。")
                # 如果解碼後是節點列表，就返回
                nodes = [node.strip() for node in decoded_content.splitlines() if node.strip().startswith(NexavorUtils.NODE_PROTOCOLS)]
                if nodes:
                     self._log(f"  -> 從 {url} 的 Base64 內容中解析出 {len(nodes)} 個節點。")
                     return nodes
            except Exception:
                # 解碼失敗，說明可能是純文本
                pass

            # 2. 如果不是 Base64，掃描原文是否包含節點
            plain_text_nodes = re.findall(NexavorUtils.PROTOCOL_REGEX, raw_content, re.I)
            if plain_text_nodes:
                self._log(f"  -> {url} 為純文本格式，掃描到 {len(plain_text_nodes)} 個節點。")
                return [node.strip() for node in plain_text_nodes]

            # 3. 如果兩者都不是，則判定為無效內容
            self._log(f"  -> 警告：{url} 的內容既不是有效 Base64 訂閱，也未直接包含節點信息。將忽略此源。")
            return []
            
        except Exception as e:
            self._log(f"錯誤：處理 {url} 失敗。原因: {e}")
            return []

# --- 圖形化介面 (GUI) ---
class AggregatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("代理聚合器 v5.0.0")
        self.root.geometry("850x850")
        
        self.internal_github_token = "github_pat_"
        self.gui_queue = queue.Queue()
        self.stop_task_event = threading.Event()
        self.thread_lock = threading.Lock()
        self.executor = None
        self.found_links = set()
        self.full_result_text = ""
        
        self.aggregator = RealProxyAggregator(self.gui_queue)
        
        self._setup_ui()
        self.process_gui_queue()

    def _setup_ui(self):
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.pack(fill="both", expand=True)
        main_frame.columnconfigure(0, weight=1); main_frame.rowconfigure(4, weight=1); main_frame.rowconfigure(5, weight=1)

        search_frame = ttk.LabelFrame(main_frame, text="線上搜索訂閱", padding="10")
        search_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        search_frame.columnconfigure(1, weight=1)

        ttk.Label(search_frame, text="GitHub Token:").grid(row=0, column=0, padx=(0, 10), pady=(0, 5), sticky='w')
        self.github_token_entry = PlaceholderEntry(search_frame, "（點擊此處，搜索無結果時可嘗試粘貼自己的Token）")
        self.github_token_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=(0, 5))
        
        ttk.Label(search_frame, text="搜索關鍵字 (,):").grid(row=1, column=0, padx=(0, 10), pady=(5, 5), sticky='w')
        self.search_query_entry = ttk.Entry(search_frame)
        self.search_query_entry.insert(0, "clash,quantumultx,v2ray,sub,SSR,vmess,trojan,vless,SS,hysteri,节点")
        self.search_query_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(5, 5))
        
        ttk.Label(search_frame, text="搜索頁數 (1-10):").grid(row=2, column=0, padx=(0, 10), pady=(5, 0), sticky='w')
        self.pages_spinbox = ttk.Spinbox(search_frame, from_=1, to=10, width=5)
        self.pages_spinbox.set(2)
        self.pages_spinbox.grid(row=2, column=1, sticky='w', pady=(5, 0))
        
        search_buttons_frame = ttk.Frame(search_frame)
        search_buttons_frame.grid(row=0, column=3, rowspan=3, padx=(15, 0), sticky='ns')
        self.search_button = ttk.Button(search_buttons_frame, text="開始並發搜索", command=self.run_search_thread)
        self.search_button.pack(fill='x', expand=True, ipady=5)
        self.stop_task_button = ttk.Button(search_buttons_frame, text="中止任務", command=self.stop_task, state='disabled')
        self.stop_task_button.pack(fill='x', expand=True, ipady=5, pady=(5,0))
        
        sub_frame = ttk.LabelFrame(main_frame, text="訂閱連結 / 直接節點 (一行一個)", padding="5")
        sub_frame.grid(row=1, column=0, sticky="ew", pady=(0, 5))
        self.sub_links_text = scrolledtext.ScrolledText(sub_frame, height=8, width=100, relief='solid', bd=1)
        self.sub_links_text.pack(fill='x', expand=True)

        proxy_frame = ttk.LabelFrame(main_frame, text="代理伺服器 (用於訪問訂閱和搜索)", padding="5")
        proxy_frame.grid(row=2, column=0, sticky="ew", pady=5)
        proxy_line_frame = ttk.Frame(proxy_frame); proxy_line_frame.pack(fill='x', expand=True, pady=2)
        self.proxy_enabled = tk.BooleanVar(value=False)
        self.proxy_checkbox = ttk.Checkbutton(proxy_line_frame, text="啟用代理", variable=self.proxy_enabled); self.proxy_checkbox.pack(side='left', padx=(0, 10))
        ttk.Label(proxy_line_frame, text="地址:").pack(side='left', padx=(0, 5))
        self.proxy_entry = ttk.Entry(proxy_line_frame); self.proxy_entry.pack(side='left', fill='x', expand=True); self.proxy_entry.insert(tk.END, "http://127.0.0.1:10809")

        control_frame = ttk.Frame(main_frame); control_frame.grid(row=3, column=0, pady=10)
        self.run_button = ttk.Button(control_frame, text="執行聚合處理", command=self.run_processing_thread); self.run_button.pack()

        log_frame = ttk.LabelFrame(main_frame, text="處理日誌", padding="5"); log_frame.grid(row=4, column=0, sticky="nsew", pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD); self.log_text.pack(fill='both', expand=True)
        
        result_frame = ttk.LabelFrame(main_frame, text="結果預覽 (只顯示前10條)", padding="5"); result_frame.grid(row=5, column=0, sticky="nsew", pady=5)
        result_header = ttk.Frame(result_frame); result_header.pack(fill='x', anchor='n', pady=(0, 5))
        self.save_button = ttk.Button(result_header, text="儲存為檔案...", command=self.save_result_to_file); self.save_button.pack(side='right', anchor='ne')
        ttk.Label(result_header, text="通用訂閱格式 (Base64)").pack(side='left', anchor='nw')
        self.result_text = scrolledtext.ScrolledText(result_frame, wrap=tk.WORD); self.result_text.pack(fill='both', expand=True)

        self._setup_context_menus()

    def _setup_context_menus(self):
        TextWidgetContextMenu(self.github_token_entry)
        TextWidgetContextMenu(self.search_query_entry)
        TextWidgetContextMenu(self.sub_links_text)
        TextWidgetContextMenu(self.proxy_entry)
        TextWidgetContextMenu(self.result_text)

    def process_gui_queue(self):
        try:
            while not self.gui_queue.empty():
                msg_type, data = self.gui_queue.get_nowait()
                if msg_type == 'log':
                    self._append_log(data)
                elif msg_type == 'found_links':
                    self.found_links.update(data)
                elif msg_type == 'store_and_display_preview':
                    self.full_result_text = data
                    self._display_preview()
                elif msg_type == 'task_done':
                    was_stopped = self.stop_task_event.is_set()
                    self.set_buttons_state(is_running=False)

                    if data == 'search':
                        if self.found_links:
                            self._update_sub_links_text()
                            msg = f"搜索已中止。已將找到的 {len(self.found_links)} 個連結輸出。" if was_stopped else f"搜索完成，已將 {len(self.found_links)} 個連結填入上方。請點擊「執行聚合處理」。"
                            self._append_log(f"\n>>> {msg} <<<")
                        elif was_stopped:
                            self._append_log("\n>>> 搜索已中止，未找到任何連結。<<<")
                        
                    elif data == 'process':
                        msg = "聚合處理已中止" if was_stopped else "聚合處理完成"
                        self._append_log(f"\n=== {msg} ===")
        finally:
            self.root.after(100, self.process_gui_queue)

    def _display_preview(self):
        self.result_text.delete('1.0', tk.END)
        if not self.full_result_text:
            return
            
        try:
            decoded_bytes = base64.b64decode(self.full_result_text)
            full_text = decoded_bytes.decode('utf-8')
            
            lines = full_text.splitlines()
            preview_lines = lines[:10]
            
            preview_text_content = "\n".join(preview_lines)
            preview_b64 = base64.b64encode(preview_text_content.encode('utf-8')).decode('utf-8')
            
            display_text = (
                f"{preview_b64}\n\n"
                f"--- (以上為前 {len(preview_lines)} 條節點預覽) ---\n"
                f"--- (共 {len(lines)} 條完整結果已儲存，請點擊“儲存為檔案...”) ---"
            )
            
            self.result_text.insert('1.0', display_text)
            
        except Exception as e:
            self.result_text.insert('1.0', f"生成預覽失敗: {e}")

    def _append_log(self, message):
        self.log_text.config(state='normal')
        self.log_text.insert(tk.END, message + "\n")
        self.log_text.config(state='disabled')
        self.log_text.see(tk.END)

    def _update_sub_links_text(self):
        self.sub_links_text.delete('1.0', tk.END)
        self.sub_links_text.insert('1.0', "\n".join(sorted(list(self.found_links))))

    def set_buttons_state(self, is_running):
        state = 'disabled' if is_running else 'normal'
        self.search_button.config(state=state)
        self.run_button.config(state=state)
        self.stop_task_button.config(state='normal' if is_running else 'disabled')

    def start_task(self, target_func, task_name):
        self.set_buttons_state(is_running=True)
        self._append_log(f"--- {task_name}任務開始 ---")
        
        self.stop_task_event.clear()
        self.executor = ThreadPoolExecutor(max_workers=20)
        
        thread = threading.Thread(target=target_func, daemon=True)
        thread.start()

    def run_search_thread(self):
        self.log_text.config(state='normal'); self.log_text.delete('1.0', tk.END); self.log_text.config(state='disabled')
        self.sub_links_text.delete('1.0', tk.END)
        self.result_text.delete('1.0', tk.END)
        self.found_links.clear()
        self.full_result_text = ""
        self.start_task(self._search_worker, "搜索")

    def run_processing_thread(self):
        self.log_text.config(state='normal'); self.log_text.delete('1.0', tk.END); self.log_text.config(state='disabled')
        self.result_text.delete('1.0', tk.END)
        self.full_result_text = ""
        self.start_task(self._processing_worker, "聚合")

    def stop_task(self):
        self._append_log("\n正在發送中止信號... 請等待當前線程結束。")
        self.stop_task_event.set()
        if self.executor:
            self.executor.shutdown(wait=False, cancel_futures=True if sys.version_info >= (3, 9) else False)
        self.stop_task_button.config(state='disabled')

    def _search_worker(self):
        try:
            user_token = self.github_token_entry.get_real_text().strip()
            
            if user_token:
                github_token = user_token
                self.gui_queue.put(('log', f'正在使用您提供的Token (長度: {len(github_token)}): {github_token[:5]}...'))
            else:
                github_token = self.internal_github_token.strip()
                self.gui_queue.put(('log', f'檢測到Token為空，使用內置Token (長度: {len(github_token)}): {github_token[:5]}...'))

            queries = [q.strip() for q in self.search_query_entry.get().strip().split(',') if q.strip()]
            pages = int(self.pages_spinbox.get())
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None

            if not queries:
                self.gui_queue.put(('log', "錯誤：請至少輸入一個搜索關鍵字。")); return

            finder = DynamicSubscriptionFinder(self.gui_queue, proxy, self.stop_task_event, self.thread_lock)
            finder.find(self.executor, github_token, queries, pages)

        except Exception as e:
            self.gui_queue.put(('log', f"搜索過程中發生嚴重錯誤: {e}\n{traceback.format_exc()}"))
        finally:
            if self.executor: self.executor.shutdown(wait=True)
            self.gui_queue.put(('task_done', 'search'))

    def _processing_worker(self):
        try:
            input_lines = [u.strip() for u in self.sub_links_text.get('1.0', tk.END).strip().split('\n') if u.strip()]
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None
            
            direct_nodes = {line for line in input_lines if line.startswith(NexavorUtils.NODE_PROTOCOLS)}
            http_links = [line for line in input_lines if line.startswith(('http://', 'https://'))]

            if not direct_nodes and not http_links:
                self.gui_queue.put(('log', "錯誤：請至少輸入一個訂閱連結或節點。")); return

            unique_nodes = set(direct_nodes)
            if unique_nodes:
                self.gui_queue.put(('log', f"已直接識別到 {len(unique_nodes)} 個節點。"))

            if http_links:
                self.gui_queue.put(('log', f"開始並發下載 {len(http_links)} 個訂閱鏈接..."))
                future_to_url = {self.executor.submit(self.aggregator.fetch_and_parse_url, url, proxy): url for url in http_links}
                
                for i, future in enumerate(as_completed(future_to_url)):
                    if self.stop_task_event.is_set(): break
                    try:
                        nodes_from_url = future.result()
                        if nodes_from_url:
                            unique_nodes.update(nodes_from_url)
                        if (i + 1) % 10 == 0 or (i + 1) == len(http_links):
                            self.gui_queue.put(('log', f"聚合進度: {i+1}/{len(http_links)} | 當前唯一節點: {len(unique_nodes)}"))
                    except Exception as e:
                        self.gui_queue.put(('log', f"一個聚合子任務失敗: {e}"))
            
            if self.stop_task_event.is_set(): return

            self.gui_queue.put(('log', "\n所有下載任務完成。開始生成最終結果..."))
            if not unique_nodes:
                self.gui_queue.put(('log', "未找到任何有效節點。")); return
            
            self.gui_queue.put(('log', f"總共找到 {len(unique_nodes)} 個唯一節點。正在進行Base64編碼..."))
            
            final_text = "\n".join(sorted(list(unique_nodes)))
            
            self.gui_queue.put(('log', f"用於編碼的文本前50字符: {repr(final_text[:50])}"))
            
            final_result = base64.b64encode(final_text.encode('utf-8')).decode('utf-8')
            
            self.gui_queue.put(('log', "編碼完成！正在更新結果預覽..."))
            self.gui_queue.put(('store_and_display_preview', final_result))

        except Exception as e:
            self.gui_queue.put(('log', f"聚合過程中發生嚴重錯誤: {e}\n{traceback.format_exc()}"))
        finally:
            if self.executor: self.executor.shutdown(wait=True)
            self.gui_queue.put(('task_done', 'process'))

    def save_result_to_file(self):
        content = self.full_result_text.strip()
        if not content:
            messagebox.showwarning("內容為空", "沒有可以儲存的內容。")
            return
        
        file_path = filedialog.asksaveasfilename(
            title="儲存訂閱檔案", 
            defaultextension=".txt", 
            filetypes=[("文字檔案", "*.txt"), ("所有檔案", "*.*")]
        )
        if not file_path: return
        
        try:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            messagebox.showinfo("儲存成功", f"訂閱檔案已成功儲存至：\n{file_path}")
        except Exception as e:
            messagebox.showerror("儲存失敗", f"儲存檔案時發生錯誤：\n{e}")

if __name__ == "__main__":
    root = tk.Tk()
    app = AggregatorApp(root)
    root.mainloop()