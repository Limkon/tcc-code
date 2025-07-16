# -*- coding: utf-8 -*-
import sys, os

# 添加 libs 文件夹支持 PyInstaller 打包
if getattr(sys, 'frozen', False):
    base_path = sys._MEIPASS
else:
    base_path = os.path.dirname(__file__)
libs_path = os.path.join(base_path, "libs")
sys.path.insert(0, libs_path)

import tkinter as tk
from tkinter import scrolledtext, messagebox, ttk, filedialog
import base64
import time
import threading
import requests
import certifi
import json
import re
import traceback
from concurrent.futures import ThreadPoolExecutor, as_completed
import queue

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
                self.menu.entryconfig("剪切", state=tk.NORMAL)
                self.menu.entryconfig("复制", state=tk.NORMAL)
            else:
                self.menu.entryconfig("剪切", state=tk.DISABLED)
                self.menu.entryconfig("复制", state=tk.DISABLED)
        except tk.TclError:
            self.menu.entryconfig("剪切", state=tk.DISABLED)
            self.menu.entryconfig("复制", state=tk.DISABLED)
        try:
            if self.widget.clipboard_get():
                self.menu.entryconfig("粘贴", state=tk.NORMAL)
        except tk.TclError:
            self.menu.entryconfig("粘贴", state=tk.DISABLED)
        self.menu.tk_popup(event.x_root, event.y_root)

    def cut(self):
        self.widget.event_generate("<<Cut>>")

    def copy(self):
        self.widget.event_generate("<<Copy>>")

    def paste(self):
        self.widget.event_generate("<<Paste>>")

    def select_all(self):
        if isinstance(self.widget, (ttk.Entry, tk.Entry)):
            self.widget.select_range(0, tk.END)
        elif isinstance(self.widget, (tk.Text, scrolledtext.ScrolledText)):
            self.widget.tag_add("sel", "1.0", "end")

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
        return "" if self.is_placeholder else super().get()

# --- NexavorUtils 辅助函数 ---
class NexavorUtils:
    USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"
    DEFAULT_HTTP_HEADERS = {
        "User-Agent": USER_AGENT,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"
    }
    NODE_PROTOCOLS = ("vmess://", "ss://", "ssr://", "trojan://", "vless://", "tuic://", "hysteria://")
    PROTOCOL_REGEX = r"(?:vmess|trojan|ss|ssr|vless|tuic|hysteria)://[a-zA-Z0-9:.?+=@%&#_\-/]{10,}"

    @staticmethod
    def http_get(url, headers=None, params=None, retry=3, proxy=None, timeout=15):
        if not (url.startswith('http://') or url.startswith('https://')):
            return ""
        if retry <= 0:
            return ""
        headers = headers if headers else NexavorUtils.DEFAULT_HTTP_HEADERS
        proxies = {'http': proxy, 'https': proxy} if proxy else None
        try:
            response = requests.get(url, headers=headers, params=params, proxies=proxies, timeout=timeout, verify=certifi.where())
            response.raise_for_status()
            return response.text
        except Exception:
            time.sleep(1)
            return NexavorUtils.http_get(url, headers, params, retry - 1, proxy, timeout)

# --- 并发搜索核心 ---
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
        if self.stop_event.is_set():
            return []
        self._log(f"[并发搜索] 开始处理关键字: '{search_query}'...")
        links = set()
        headers = {
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": self.utils.USER_AGENT
        }
        if token:
            headers["Authorization"] = f"Bearer {token}"

        for page in range(1, pages + 1):
            if self.stop_event.is_set():
                self._log(f"[并发搜索] 关键字 '{search_query}' 的任务已中止。")
                break

            params = {'q': search_query, 'sort': 'indexed', 'order': 'desc', 'per_page': 100, 'page': page}
            api_url = "https://api.github.com/search/code"
            try:
                content = self.utils.http_get(api_url, params=params, headers=headers, proxy=self.proxy_address)
                if not content:
                    continue
                data = json.loads(content)
                items = data.get("items", [])
                if not items:
                    break

                for item in items:
                    if self.stop_event.is_set():
                        break
                    html_url = item.get("html_url")
                    if html_url:
                        # 转换为原始文件链接
                        links.add(html_url.replace("github.com", "raw.githubusercontent.com").replace("/blob/", "/"))
                time.sleep(2)
            except Exception as e:
                self._log(f"[并发搜索] 处理关键字 '{search_query}' 第 {page} 页时出错: {e}")
                break

        self._log(f"[并发搜索] 关键字 '{search_query}' 完成，找到 {len(links)} 个潜在链接。")
        return list(links)

    def fetch_and_extract_from_url(self, file_url):
        if self.stop_event.is_set():
            return []

        content = self.utils.http_get(file_url, proxy=self.proxy_address)
        if not content:
            return []

        subscriptions = self.extract_subscriptions_from_content(content)
        if subscriptions:
            with self.lock:
                self._log(f"  => 从 {file_url} 找到 {len(subscriptions)} 个链接。")
        return subscriptions

    def extract_subscriptions_from_content(self, content):
        if not content:
            return []
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
            if self.stop_event.is_set():
                break
            try:
                potential_files.update(future.result())
            except Exception as e:
                self._log(f"一个关键字搜索任务失败: {e}")

        if self.stop_event.is_set():
            return

        self._log(f"\n=== 第二阶段: 从 {len(potential_files)} 个文件中并发提取订阅链接... ===")
        if not potential_files:
            self._log("未找到任何可能的文件。")
            return

        future_to_url = {executor.submit(self.fetch_and_extract_from_url, url): url for url in potential_files}
        for i, future in enumerate(as_completed(future_to_url)):
            if self.stop_event.is_set():
                break
            if (i + 1) % 20 == 0 or (i + 1) == len(potential_files):
                self._log(f"提取进度: {i+1}/{len(potential_files)}")
            try:
                subscriptions = future.result()
                if subscriptions:
                    self.gui_queue.put(('found_links', subscriptions))
            except Exception as e:
                self._log(f"一个文件提取任务失败: {e}")

# --- 后端处理核心 ---
class RealProxyAggregator:
    def __init__(self, gui_queue):
        self.gui_queue = gui_queue

    def _log(self, msg):
        self.gui_queue.put(('log', msg))

    def fetch_and_parse_url(self, url, proxy_address=None):
        self._log(f"正在下载订阅: {url}")
        try:
            raw_content = NexavorUtils.http_get(url, proxy=proxy_address)
            if not raw_content:
                raise ValueError("下载内容为空")

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

# --- GUI 应用 ---
class AggregatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("代理聚合器 v5.0.0")
        self.root.geometry("850x850")

        self.internal_github_token = "github_pat_XXX"
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
        main_frame.columnconfigure(0, weight=1)
        main_frame.rowconfigure(4, weight=1)
        main_frame.rowconfigure(5, weight=1)

        search_frame = ttk.LabelFrame(main_frame, text="在线搜索订阅", padding="10")
        search_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        search_frame.columnconfigure(1, weight=1)

        ttk.Label(search_frame, text="GitHub Token:").grid(row=0, column=0, padx=(0, 10), pady=(0, 5), sticky='w')
        self.github_token_entry = PlaceholderEntry(search_frame, "（点击此处，搜索无结果时可尝试粘贴自己的Token）")
        self.github_token_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=(0, 5))

        ttk.Label(search_frame, text="搜索关键词 (,):").grid(row=1, column=0, padx=(0, 10), pady=(5, 5), sticky='w')
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
        self.stop_task_button.pack(fill='x', expand=True, ipady=5, pady=(5, 0))

        sub_frame = ttk.LabelFrame(main_frame, text="订阅链接 / 直接节点 (一行一个)", padding="5")
        sub_frame.grid(row=1, column=0, sticky="ew", pady=(0, 5))
        self.sub_links_text = scrolledtext.ScrolledText(sub_frame, height=8, width=100, relief='solid', bd=1)
        self.sub_links_text.pack(fill='x', expand=True)

        proxy_frame = ttk.LabelFrame(main_frame, text="代理服务器 (用于访问订阅和搜索)", padding="5")
        proxy_frame.grid(row=2, column=0, sticky="ew", pady=5)
        proxy_line_frame = ttk.Frame(proxy_frame)
        proxy_line_frame.pack(fill='x', expand=True, pady=2)
        self.proxy_enabled = tk.BooleanVar(value=False)
        self.proxy_checkbox = ttk.Checkbutton(proxy_line_frame, text="启用代理", variable=self.proxy_enabled)
        self.proxy_checkbox.pack(side='left', padx=(0, 10))
        ttk.Label(proxy_line_frame, text="地址:").pack(side='left', padx=(0, 5))
        self.proxy_entry = ttk.Entry(proxy_line_frame)
        self.proxy_entry.pack(side='left', fill='x', expand=True)
        self.proxy_entry.insert(tk.END, "http://127.0.0.1:10809")

        control_frame = ttk.Frame(main_frame)
        control_frame.grid(row=3, column=0, pady=10)
        self.run_button = ttk.Button(control_frame, text="执行聚合处理", command=self.run_processing_thread)
        self.run_button.pack()

        log_frame = ttk.LabelFrame(main_frame, text="处理日志", padding="5")
        log_frame.grid(row=4, column=0, sticky="nsew", pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD)
        self.log_text.pack(fill='both', expand=True)

        result_frame = ttk.LabelFrame(main_frame, text="结果预览 (只显示前10条)", padding="5")
        result_frame.grid(row=5, column=0, sticky="nsew", pady=5)
        self.result_text = scrolledtext.ScrolledText(result_frame, state='disabled', wrap=tk.WORD)
        self.result_text.pack(fill='both', expand=True)

        # 右键菜单支持
        TextWidgetContextMenu(self.sub_links_text)
        TextWidgetContextMenu(self.log_text)
        TextWidgetContextMenu(self.result_text)

    def stop_task(self):
        self.stop_task_event.set()
        self._log("任务中止信号已发送。")
        self.stop_task_button.config(state='disabled')

    def _log(self, message):
        self.gui_queue.put(('log', message))

    def process_gui_queue(self):
        try:
            while True:
                item = self.gui_queue.get_nowait()
                if not item:
                    continue
                tag, content = item
                if tag == 'log':
                    self.log_text.config(state='normal')
                    self.log_text.insert(tk.END, content + "\n")
                    self.log_text.see(tk.END)
                    self.log_text.config(state='disabled')
                elif tag == 'found_links':
                    for link in content:
                        if link not in self.found_links:
                            self.found_links.add(link)
                            self.sub_links_text.insert(tk.END, link + "\n")
                else:
                    pass
                self.gui_queue.task_done()
        except queue.Empty:
            pass
        self.root.after(200, self.process_gui_queue)

    def run_search_thread(self):
        if self.executor:
            self._log("另一个任务正在运行，请先停止后再试。")
            return
        self.stop_task_event.clear()
        self.found_links.clear()
        self.sub_links_text.delete('1.0', tk.END)
        self.log_text.config(state='normal')
        self.log_text.delete('1.0', tk.END)
        self.log_text.config(state='disabled')
        queries = self.search_query_entry.get().strip()
        if not queries:
            messagebox.showwarning("提示", "请输入搜索关键字！")
            return
        queries = [q.strip() for q in queries.split(",") if q.strip()]
        pages = int(self.pages_spinbox.get())
        github_token = self.github_token_entry.get_real_text()
        if not github_token:
            github_token = self.internal_github_token
        proxy = self.proxy_entry.get() if self.proxy_enabled.get() else None
        self.executor = ThreadPoolExecutor(max_workers=5)
        self.stop_task_button.config(state='normal')
        self.search_button.config(state='disabled')
        finder = DynamicSubscriptionFinder(self.gui_queue, proxy_address=proxy, stop_event=self.stop_task_event, lock=self.thread_lock)

        def task():
            try:
                finder.find(self.executor, github_token, queries, pages)
            finally:
                self.executor.shutdown(wait=False)
                self.executor = None
                self.stop_task_event.clear()
                self.root.after(0, lambda: [self.search_button.config(state='normal'), self.stop_task_button.config(state='disabled')])
                self._log("并发搜索任务结束。")

        threading.Thread(target=task, daemon=True).start()

    def run_processing_thread(self):
        if self.executor:
            self._log("另一个任务正在运行，请先停止后再试。")
            return
        self.stop_task_event.clear()
        self.log_text.config(state='normal')
        self.log_text.delete('1.0', tk.END)
        self.log_text.config(state='disabled')
        self.result_text.config(state='normal')
        self.result_text.delete('1.0', tk.END)
        self.result_text.config(state='disabled')

        raw_links = self.sub_links_text.get('1.0', tk.END).strip().splitlines()
        unique_links = list({link.strip() for link in raw_links if link.strip()})

        if not unique_links:
            messagebox.showwarning("提示", "请输入订阅链接或节点！")
            return

        proxy = self.proxy_entry.get() if self.proxy_enabled.get() else None
        self.executor = ThreadPoolExecutor(max_workers=10)
        self.stop_task_button.config(state='normal')
        self.run_button.config(state='disabled')
        aggregator = self.aggregator
        stop_event = self.stop_task_event

        def task():
            aggregated_nodes = []
            futures = []
            for url in unique_links:
                if stop_event.is_set():
                    break
                futures.append(self.executor.submit(aggregator.fetch_and_parse_url, url, proxy))

            for future in as_completed(futures):
                if stop_event.is_set():
                    break
                try:
                    nodes = future.result()
                    if nodes:
                        aggregated_nodes.extend(nodes)
                        self.gui_queue.put(('log', f"当前总节点数: {len(aggregated_nodes)}"))
                except Exception as e:
                    self.gui_queue.put(('log', f"任务异常: {e}"))

            if aggregated_nodes:
                self.full_result_text = "\n".join(sorted(set(aggregated_nodes)))
                preview = "\n".join(sorted(set(aggregated_nodes))[:10])
                self.gui_queue.put(('log', f"聚合完成，节点总数: {len(aggregated_nodes)}"))
                self.root.after(0, lambda: self.show_result(preview))
            else:
                self.gui_queue.put(('log', "没有找到有效节点。"))

            self.executor.shutdown(wait=False)
            self.executor = None
            self.stop_task_event.clear()
            self.root.after(0, lambda: [self.run_button.config(state='normal'), self.stop_task_button.config(state='disabled')])
            self._log("聚合处理任务结束。")

        threading.Thread(target=task, daemon=True).start()

    def show_result(self, text):
        self.result_text.config(state='normal')
        self.result_text.delete('1.0', tk.END)
        self.result_text.insert(tk.END, text)
        self.result_text.config(state='disabled')

if __name__ == "__main__":
    root = tk.Tk()
    app = AggregatorApp(root)
    root.mainloop()
