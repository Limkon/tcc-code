# -*- coding: utf-8 -*-

# --- Windows 7 兼容性说明 ---
# 1. Python 版本: 此脚本建议使用 Python 3.8.x 版本运行，因为这是官方支持 Windows 7 的最后一个主要 Python 版本。
# 2. 安装依赖库: 为了确保在 Windows 7 上能成功发起 HTTPS 网络请求 (因其内置安全凭证可能过旧)，
#    强烈建议安装 'certifi' 库。请使用以下指令安装所有必要的函数库：
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
import socket # (新增) 导入 socket 库用于测速

# 为了支持SOCKS代理，需要安装 PySocks
try:
    import socks
except ImportError:
    class FakeSocks:
        def set_default_proxy(self, *args, **kwargs):
            pass
    sys.modules['socks'] = FakeSocks()


# --- 通用右键菜单功能 ---
class TextWidgetContextMenu:
    def __init__(self, widget):
        self.widget = widget
        self.menu = tk.Menu(widget, tearoff=0)
        self.menu.add_command(label="剪切", command=self.cut)
        self.menu.add_command(label="复制", command=self.copy)
        self.menu.add_command(label="粘贴", command=self.paste)
        self.menu.add_separator()
        self.menu.add_command(label="全选", command=self.select_all)
        widget.bind("<Button-3>", self.show_menu)

    def show_menu(self, event):
        try:
            if self.widget.selection_get():
                self.menu.entryconfig("剪切", state=tk.NORMAL); self.menu.entryconfig("复制", state=tk.NORMAL)
            else:
                self.menu.entryconfig("剪切", state=tk.DISABLED); self.menu.entryconfig("复制", state=tk.DISABLED)
        except tk.TclError:
            self.menu.entryconfig("剪切", state=tk.DISABLED); self.menu.entryconfig("复制", state=tk.DISABLED)
        try:
            if self.widget.clipboard_get(): self.menu.entryconfig("粘贴", state=tk.NORMAL)
        except tk.TclError:
            self.menu.entryconfig("粘贴", state=tk.DISABLED)
        self.menu.tk_popup(event.x_root, event.y_root)

    def cut(self): self.widget.event_generate("<<Cut>>")
    def copy(self): self.widget.event_generate("<<Copy>>")
    def paste(self): self.widget.event_generate("<<Paste>>")
    def select_all(self):
        if isinstance(self.widget, (ttk.Entry, tk.Entry)): self.widget.select_range(0, tk.END)
        elif isinstance(self.widget, (tk.Text, scrolledtext.ScrolledText)): self.widget.tag_add("sel", "1.0", "end")

# --- 带占位符的输入框 ---
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

# --- 从 nexavor/aggregator 项目移植的辅助函数 ---
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

# --- 整合后的并发搜索核心 ---
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
        self._log(f"[并发搜索] 开始处理关键字: '{search_query}'...")
        links = set()
        headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28", "User-Agent": self.utils.USER_AGENT}
        if token: headers["Authorization"] = f"Bearer {token}"
        
        for page in range(1, pages + 1):
            if self.stop_event.is_set(): self._log(f"[并发搜索] 关键字 '{search_query}' 的任务已中止。"); break
            
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
                self._log(f"[并发搜索] 处理关键字 '{search_query}' 第 {page} 页时出错: {e}"); break
        
        self._log(f"[并发搜索] 关键字 '{search_query}' 完成，找到 {len(links)} 个潜在链接。")
        return list(links)

    def fetch_and_extract_from_url(self, file_url):
        if self.stop_event.is_set(): return []
        
        content = self.utils.http_get(file_url, proxy=self.proxy_address)
        if not content: return []

        subscriptions = self.extract_subscriptions_from_content(content)
        if subscriptions:
            with self.lock:
                self._log(f"  => 从 {file_url} 找到 {len(subscriptions)} 个链接。")
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
        self._log(f"=== 第一阶段: 开始并发搜索 {len(queries)} 个关键字 ===")
        if not github_token: 
            self._log("[GitHub搜索] 严重警告：未提供任何有效 GitHub Token，搜索请求将极大概率失败！")
        
        future_to_query = {executor.submit(self.search_github_for_keyword, github_token, f'"{query}" in:file', pages): query for query in queries}
        for future in as_completed(future_to_query):
            if self.stop_event.is_set(): break
            try: potential_files.update(future.result())
            except Exception as e: self._log(f"一个关键字搜索任务失败: {e}")
        
        if self.stop_event.is_set(): return
        
        self._log(f"\n=== 第二阶段: 从 {len(potential_files)} 个文件中并发提取订阅链接... ===")
        if not potential_files: self._log("未找到任何可能的文件。"); return
            
        future_to_url = {executor.submit(self.fetch_and_extract_from_url, url): url for url in potential_files}
        for i, future in enumerate(as_completed(future_to_url)):
            if self.stop_event.is_set(): break
            if (i + 1) % 20 == 0 or (i + 1) == len(potential_files):
                self._log(f"提取进度: {i+1}/{len(potential_files)}")
            try:
                subscriptions = future.result()
                if subscriptions: self.gui_queue.put(('found_links', subscriptions))
            except Exception as e: self._log(f"一个文件提取任务失败: {e}")

# --- 后端处理核心 (集成测速) ---
class RealProxyAggregator:
    def __init__(self, gui_queue):
        self.gui_queue = gui_queue

    def _log(self, msg): self.gui_queue.put(('log', msg))

    def fetch_and_parse_url(self, url, proxy_address=None):
        self._log(f"正在下载订阅: {url}")
        try:
            raw_content = NexavorUtils.http_get(url, proxy=proxy_address)
            if not raw_content: raise ValueError("下载内容为空")
            
            # --- 内容验证核心逻辑 ---
            # 1. 优先尝试 Base64 解码
            try:
                decoded_content = base64.b64decode(raw_content.strip()).decode('utf-8')
                self._log(f"  -> {url} 解码为 Base64 成功。")
                # 如果解码后是节点列表，就返回
                nodes = [node.strip() for node in decoded_content.splitlines() if node.strip().startswith(NexavorUtils.NODE_PROTOCOLS)]
                if nodes:
                     self._log(f"  -> 从 {url} 的 Base64 内容中解析出 {len(nodes)} 个节点。")
                     return nodes
            except Exception:
                # 解码失败，说明可能是纯文本
                pass

            # 2. 如果不是 Base64，扫描原文是否包含节点
            plain_text_nodes = re.findall(NexavorUtils.PROTOCOL_REGEX, raw_content, re.I)
            if plain_text_nodes:
                self._log(f"  -> {url} 为纯文本格式，扫描到 {len(plain_text_nodes)} 个节点。")
                return [node.strip() for node in plain_text_nodes]

            # 3. 如果两者都不是，则判定为无效内容
            self._log(f"  -> 警告：{url} 的内容既不是有效 Base64 订阅，也未直接包含节点信息。将忽略此源。")
            return []
            
        except Exception as e:
            self._log(f"错误：处理 {url} 失败。原因: {e}")
            return []

    @staticmethod
    def _tcp_ping(address, port, timeout=2):
        """(新增) 尝试TCP连接并返回延迟(ms)，失败返回 float('inf')"""
        try:
            port_int = int(port)
            if port_int <= 0 or port_int > 65535:
                return float('inf')
        except (ValueError, TypeError):
            return float('inf')

        try:
            # 自动解析 IPv4/IPv6
            addr_info_list = socket.getaddrinfo(address, port_int, 0, socket.SOCK_STREAM)
            
            if not addr_info_list:
                return float('inf') # 域名无法解析

            # 只尝试第一个返回的地址
            target_addr_info = addr_info_list[0]
            af, socktype, proto, canonname, sa = target_addr_info
            
            start_time = time.time()
            with socket.socket(af, socktype, proto) as sock:
                sock.settimeout(timeout)
                sock.connect(sa)
                end_time = time.time()
                return (end_time - start_time) * 1000  # 返回毫秒
        except (socket.error, socket.timeout, OverflowError, OSError): # OSError for getaddrinfo fails
            return float('inf')
        except Exception:
            # 其他未知错误
            return float('inf')

    @staticmethod
    def _parse_node_link(link):
        """(新增) 解析节点链接，返回 (address, port) 或 None"""
        try:
            if link.startswith("vmess://"):
                try:
                    # 1. VMess
                    decoded = base64.b64decode(link[8:]).decode('utf-8')
                    data = json.loads(decoded)
                    return data.get('add'), data.get('port')
                except Exception:
                    return None # 解码或JSON解析失败

            elif link.startswith(("vless://", "trojan://")):
                # 2. VLess / Trojan (标准URL格式)
                parsed_url = urllib.parse.urlparse(link)
                # hostname 会自动处理 [::1] 为 ::1
                return parsed_url.hostname, parsed_url.port

            elif link.startswith("ss://"):
                # 3. SS (Shadowsocks)
                try:
                    parsed_url = urllib.parse.urlparse(link)
                    # 尝试1: URL 格式 (e.g., ss://method:pass@host:port#tag)
                    if parsed_url.username and parsed_url.hostname and parsed_url.port:
                        return parsed_url.hostname, parsed_url.port

                    # 尝试2: Base64 格式 (ss://Base64String#Tag)
                    # 此时 base64 字符串会被错误地解析为 netloc
                    main_part = parsed_url.netloc
                    
                    if main_part:
                        # 需要补全 padding
                        padding = '=' * (-len(main_part) % 4)
                        decoded = base64.b64decode(main_part + padding).decode('utf-8')
                        # 格式: method:password@server:port
                        at_parts = decoded.split('@')
                        if len(at_parts) == 2:
                            server_part = at_parts[1]
                            host_port = server_part.rsplit(':', 1) # 从右侧分割，以支持 IPv6 [host]:port
                            if len(host_port) == 2:
                                host = host_port[0].strip('[]') # 去除 IPv6 方括号
                                return host, host_port[1]
                except Exception:
                    return None # 解析失败
                return None # 两种格式都未匹配

            elif link.startswith("ssr://"):
                # 4. SSR (ShadowsocksR)
                try:
                    # SSR 使用 URL-safe Base64
                    decoded_part = link[6:]
                    padding = '=' * (-len(decoded_part) % 4)
                    decoded = base64.urlsafe_b64decode(decoded_part + padding).decode('utf-8')
                    # 格式: server:port:protocol:method:obfs:password_base64/?...
                    # SSR 格式的 server 字段不支持 IPv6
                    main_parts = decoded.split(':')
                    if len(main_parts) >= 2:
                        return main_parts[0], main_parts[1]
                except Exception:
                    return None # 解码失败
            
            return None # 未知或不支持的协议 (e.g., tuic, hysteria)
        
        except Exception:
            return None # 兜底

# --- 图形化界面 (GUI) ---
class AggregatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("代理聚合器 v5.1.0 (带测速)")
        self.root.geometry("850x900") # (修改) 增加高度以容纳测速框
        
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
        # (修改) 调整行权重以适应新布局
        main_frame.columnconfigure(0, weight=1); main_frame.rowconfigure(5, weight=1); main_frame.rowconfigure(6, weight=1)

        search_frame = ttk.LabelFrame(main_frame, text="在线搜索订阅", padding="10")
        search_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        search_frame.columnconfigure(1, weight=1)

        ttk.Label(search_frame, text="GitHub Token:").grid(row=0, column=0, padx=(0, 10), pady=(0, 5), sticky='w')
        self.github_token_entry = PlaceholderEntry(search_frame, "（点击此处，搜索无结果时可尝试粘贴自己的Token）")
        self.github_token_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=(0, 5))
        
        ttk.Label(search_frame, text="搜索关键字 (逗号隔开):").grid(row=1, column=0, padx=(0, 10), pady=(5, 5), sticky='w')
        self.search_query_entry = ttk.Entry(search_frame)
        self.search_query_entry.insert(0, "clash,quantumultx,v2ray,sub,SSR,vmess,trojan,vless,SS,hysteri,节点")
        self.search_query_entry.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(5, 5))
        
        ttk.Label(search_frame, text="搜索页数 (1-10):").grid(row=2, column=0, padx=(0, 10), pady=(5, 0), sticky='w')
        self.pages_spinbox = ttk.Spinbox(search_frame, from_=1, to=10, width=5)
        self.pages_spinbox.set(2)
        self.pages_spinbox.grid(row=2, column=1, sticky='w', pady=(5, 0))
        
        search_buttons_frame = ttk.Frame(search_frame)
        search_buttons_frame.grid(row=0, column=3, rowspan=3, padx=(15, 0), sticky='ns')
        self.search_button = ttk.Button(search_buttons_frame, text="开始并发搜索", command=self.run_search_thread)
        self.search_button.pack(fill='x', expand=True, ipady=5)
        self.stop_task_button = ttk.Button(search_buttons_frame, text="中止任务", command=self.stop_task, state='disabled')
        self.stop_task_button.pack(fill='x', expand=True, ipady=5, pady=(5,0))
        
        sub_frame = ttk.LabelFrame(main_frame, text="订阅链接 / 直接节点 (一行一个)", padding="5")
        sub_frame.grid(row=1, column=0, sticky="ew", pady=(0, 5))
        self.sub_links_text = scrolledtext.ScrolledText(sub_frame, height=8, width=100, relief='solid', bd=1)
        self.sub_links_text.pack(fill='x', expand=True)

        proxy_frame = ttk.LabelFrame(main_frame, text="代理服务器 (用于访问订阅和搜索)", padding="5")
        proxy_frame.grid(row=2, column=0, sticky="ew", pady=5)
        proxy_line_frame = ttk.Frame(proxy_frame); proxy_line_frame.pack(fill='x', expand=True, pady=2)
        self.proxy_enabled = tk.BooleanVar(value=False)
        self.proxy_checkbox = ttk.Checkbutton(proxy_line_frame, text="启用代理", variable=self.proxy_enabled); self.proxy_checkbox.pack(side='left', padx=(0, 10))
        ttk.Label(proxy_line_frame, text="地址:").pack(side='left', padx=(0, 5))
        self.proxy_entry = ttk.Entry(proxy_line_frame); self.proxy_entry.pack(side='left', fill='x', expand=True); self.proxy_entry.insert(tk.END, "http://127.0.0.1:10809")

        control_frame = ttk.Frame(main_frame); control_frame.grid(row=3, column=0, pady=10)
        self.run_button = ttk.Button(control_frame, text="执行聚合处理 (含测速)", command=self.run_processing_thread); self.run_button.pack()

        # --- (新增) 测速设置 ---
        speed_test_frame = ttk.LabelFrame(main_frame, text="测速与筛选", padding="10")
        speed_test_frame.grid(row=4, column=0, sticky="ew", pady=(0, 5))
        speed_test_frame.columnconfigure(1, weight=1)

        self.speed_test_enabled = tk.BooleanVar(value=True) # 默认开启
        self.speed_test_checkbox = ttk.Checkbutton(speed_test_frame, text="启用测速 (仅支持 Vmess/Vless/Trojan/SS/SSR)", variable=self.speed_test_enabled)
        self.speed_test_checkbox.grid(row=0, column=0, columnspan=3, sticky='w', pady=(0, 5))

        ttk.Label(speed_test_frame, text="测速超时 (秒):").grid(row=1, column=0, padx=(0, 10), sticky='w')
        self.timeout_spinbox = ttk.Spinbox(speed_test_frame, from_=1, to=10, width=5)
        self.timeout_spinbox.set(2)
        self.timeout_spinbox.grid(row=1, column=1, sticky='w')

        ttk.Label(speed_test_frame, text="并发数 (搜索/测速):").grid(row=2, column=0, padx=(0, 10), sticky='w')
        self.concurrency_spinbox = ttk.Spinbox(speed_test_frame, from_=10, to=200, increment=10, width=5)
        self.concurrency_spinbox.set(50)
        self.concurrency_spinbox.grid(row=2, column=1, sticky='w')
        # --- 测速设置结束 ---

        # (修改) 行索引 +1
        log_frame = ttk.LabelFrame(main_frame, text="处理日志", padding="5"); log_frame.grid(row=5, column=0, sticky="nsew", pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD); self.log_text.pack(fill='both', expand=True)
        
        # (修改) 行索引 +1
        result_frame = ttk.LabelFrame(main_frame, text="结果预览 (只显示前10条)", padding="5"); result_frame.grid(row=6, column=0, sticky="nsew", pady=5)
        result_header = ttk.Frame(result_frame); result_header.pack(fill='x', anchor='n', pady=(0, 5))
        self.save_button = ttk.Button(result_header, text="保存为文件...", command=self.save_result_to_file); self.save_button.pack(side='right', anchor='ne')
        ttk.Label(result_header, text="通用订阅格式 (Base64)").pack(side='left', anchor='nw')
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
                            msg = f"搜索已中止。已将找到的 {len(self.found_links)} 个链接输出。" if was_stopped else f"搜索完成，已将 {len(self.found_links)} 个链接填入上方。请点击“执行聚合处理”。"
                            self._append_log(f"\n>>> {msg} <<<")
                        elif was_stopped:
                            self._append_log("\n>>> 搜索已中止，未找到任何链接。<<<")
                        
                    elif data == 'process':
                        msg = "聚合处理已中止" if was_stopped else "聚合处理完成"
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
                f"--- (以上为前 {len(preview_lines)} 条节点预览) ---\n"
                f"--- (共 {len(lines)} 条完整结果已保存，请点击“保存为文件...”) ---"
            )
            
            self.result_text.insert('1.0', display_text)
            
        except Exception as e:
            self.result_text.insert('1.0', f"生成预览失败: {e}")

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
        self._append_log(f"--- {task_name}任务开始 ---")
        
        self.stop_task_event.clear()
        # (修改) Executor 在 worker 线程中根据 GUI 设置初始化
        # self.executor = ThreadPoolExecutor(max_workers=20) 
        
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
        self._append_log("\n正在发送中止信号... 请等待当前线程结束。")
        self.stop_task_event.set()
        if self.executor:
            # (修改) 增加 cancel_futures=True 以尽快停止 (需要 Python 3.9+)
            cancel_futures = sys.version_info >= (3, 9)
            self.executor.shutdown(wait=False, cancel_futures=cancel_futures)
        self.stop_task_button.config(state='disabled')

    def _search_worker(self):
        try:
            user_token = self.github_token_entry.get_real_text().strip()
            
            if user_token:
                github_token = user_token
                self.gui_queue.put(('log', f'正在使用您提供的Token (长度: {len(github_token)}): {github_token[:5]}...'))
            else:
                github_token = self.internal_github_token.strip()
                self.gui_queue.put(('log', f'检测到Token为空，使用内置Token (长度: {len(github_token)}): {github_token[:5]}...'))

            queries = [q.strip() for q in self.search_query_entry.get().strip().split(',') if q.strip()]
            pages = int(self.pages_spinbox.get())
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None
            
            # (新增) 读取并发数
            concurrency = int(self.concurrency_spinbox.get())
            # (修改) 根据 GUI 设置初始化 Executor
            self.executor = ThreadPoolExecutor(max_workers=concurrency)

            if not queries:
                self.gui_queue.put(('log', "错误：请至少输入一个搜索关键字。")); return

            finder = DynamicSubscriptionFinder(self.gui_queue, proxy, self.stop_task_event, self.thread_lock)
            finder.find(self.executor, github_token, queries, pages)

        except Exception as e:
            self.gui_queue.put(('log', f"搜索过程中发生严重错误: {e}\n{traceback.format_exc()}"))
        finally:
            if self.executor: self.executor.shutdown(wait=True)
            self.gui_queue.put(('task_done', 'search'))

    def _processing_worker(self):
        try:
            input_lines = [u.strip() for u in self.sub_links_text.get('1.0', tk.END).strip().split('\n') if u.strip()]
            proxy = self.proxy_entry.get().strip() if self.proxy_enabled.get() else None
            
            # (新增) 获取测速设置
            enable_speed_test = self.speed_test_enabled.get()
            test_timeout = int(self.timeout_spinbox.get())
            test_concurrency = int(self.concurrency_spinbox.get())
            
            direct_nodes = {line for line in input_lines if line.startswith(NexavorUtils.NODE_PROTOCOLS)}
            http_links = [line for line in input_lines if line.startswith(('http://', 'https://'))]

            if not direct_nodes and not http_links:
                self.gui_queue.put(('log', "错误：请至少输入一个订阅链接或节点。")); return

            unique_nodes = set(direct_nodes)
            if unique_nodes:
                self.gui_queue.put(('log', f"已直接识别到 {len(unique_nodes)} 个节点。"))

            if http_links:
                self.gui_queue.put(('log', f"开始并发下载 {len(http_links)} 个订阅链接..."))
                
                # (修改) 根据 GUI 设置初始化 Executor
                if self.executor: self.executor.shutdown(wait=False) # 关闭旧的
                self.executor = ThreadPoolExecutor(max_workers=test_concurrency)
                
                future_to_url = {self.executor.submit(self.aggregator.fetch_and_parse_url, url, proxy): url for url in http_links}
                
                for i, future in enumerate(as_completed(future_to_url)):
                    if self.stop_task_event.is_set(): break
                    try:
                        nodes_from_url = future.result()
                        if nodes_from_url:
                            unique_nodes.update(nodes_from_url)
                        if (i + 1) % 10 == 0 or (i + 1) == len(http_links):
                            self.gui_queue.put(('log', f"聚合进度: {i+1}/{len(http_links)} | 当前唯一节点: {len(unique_nodes)}"))
                    except Exception as e:
                        self.gui_queue.put(('log', f"一个聚合子任务失败: {e}"))
            
            if self.stop_task_event.is_set(): return

            self.gui_queue.put(('log', f"\n下载完成。总共找到 {len(unique_nodes)} 个唯一节点。"))
            
            final_node_list = sorted(list(unique_nodes))

            # --- (新增) 测速逻辑 ---
            if enable_speed_test and final_node_list:
                self.gui_queue.put(('log', f"=== 开始测速（并发: {test_concurrency}, 超时: {test_timeout}s） ==="))
                
                nodes_to_test = []
                parsing_errors = 0
                unsupported = 0
                
                self.gui_queue.put(('log', "步骤 1: 正在解析节点链接..."))
                for node_link in final_node_list:
                    if self.stop_task_event.is_set(): return
                    parsed = self.aggregator._parse_node_link(node_link)
                    if parsed:
                        addr, port = parsed
                        if addr and port:
                            nodes_to_test.append({'link': node_link, 'addr': addr, 'port': port})
                        else:
                            parsing_errors += 1
                    else:
                        # Hysteria, Tuic 等协议 parse_node_link 返回 None
                        unsupported += 1
                
                self.gui_queue.put(('log', f"解析完成: {len(nodes_to_test)} 个可测速, {unsupported} 个不支持, {parsing_errors} 个解析失败。"))
                
                if not nodes_to_test:
                    self.gui_queue.put(('log', "没有可测速的节点。"))
                    return # 结束任务

                self.gui_queue.put(('log', f"步骤 2: 开始并发 TCP Ping {len(nodes_to_test)} 个节点..."))
                
                # 确保 executor 存在且并发数正确
                if self.executor is None: # 如果之前没有 http_links, executor 未初始化
                     self.executor = ThreadPoolExecutor(max_workers=test_concurrency)
                
                future_to_node = {
                    self.executor.submit(self.aggregator._tcp_ping, node['addr'], node['port'], test_timeout): node['link']
                    for node in nodes_to_test
                }

                results = [] # (delay, link)
                tested_count = 0
                for future in as_completed(future_to_node):
                    if self.stop_task_event.is_set(): break
                    link = future_to_node[future]
                    try:
                        delay = future.result()
                        if delay != float('inf'):
                            results.append((delay, link))
                    except Exception as e:
                        self.gui_queue.put(('log', f"测速 {link[:30]}... 出错: {e}"))
                    
                    tested_count += 1
                    if tested_count % 20 == 0 or tested_count == len(nodes_to_test):
                         self.gui_queue.put(('log', f"测速进度: {tested_count}/{len(nodes_to_test)} | 存活: {len(results)}"))

                if self.stop_task_event.is_set(): return

                self.gui_queue.put(('log', "\n测速完成。正在按延迟排序..."))
                
                # 按延迟排序
                results.sort(key=lambda x: x[0])
                final_node_list = [link for delay, link in results]
                
                if not final_node_list:
                    self.gui_queue.put(('log', "未找到任何测速成功的节点。")); return
                
                self.gui_queue.put(('log', f"筛选完毕！共 {len(final_node_list)} 个存活节点。最快延迟: {results[0][0]:.2f} ms"))

            # --- 测速逻辑结束 ---

            elif not final_node_list:
                 self.gui_queue.put(('log', "未找到任何有效节点。")); return
            else:
                self.gui_queue.put(('log', "测速未启用。按默认顺序生成结果..."))
            
            self.gui_queue.put((('log', f"总共 {len(final_node_list)} 个节点。正在进行Base64编码...")))
            
            final_text = "\n".join(final_node_list)
            
            self.gui_queue.put(('log', f"用于编码的文本前50个字符: {repr(final_text[:50])}"))
            
            final_result = base64.b64encode(final_text.encode('utf-8')).decode('utf-8')
            
            self.gui_queue.put(('log', "编码完成！正在更新结果预览..."))
            self.gui_queue.put(('store_and_display_preview', final_result))

        except Exception as e:
            self.gui_queue.put(('log', f"聚合过程中发生严重错误: {e}\n{traceback.format_exc()}"))
        finally:
            if self.executor: self.executor.shutdown(wait=True)
            self.gui_queue.put(('task_done', 'process'))

    def save_result_to_file(self):
        content = self.full_result_text.strip()
        if not content:
            messagebox.showwarning("内容为空", "没有可以保存的内容。")
            return
        
        file_path = filedialog.asksaveasfilename(
            title="保存订阅文件", 
            defaultextension=".txt", 
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")]
        )
        if not file_path: return
        
        try:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            messagebox.showinfo("保存成功", f"订阅文件已成功保存至：\n{file_path}")
        except Exception as e:
            messagebox.showerror("保存失败", f"保存文件时发生错误：\n{e}")

if __name__ == "__main__":
    root = tk.Tk()
    app = AggregatorApp(root)
    root.mainloop()