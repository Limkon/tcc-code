import tkinter as tk
from tkinter import scrolledtext, messagebox, ttk, filedialog
import base64
import random
import time
import threading
import requests
import sys

# 為了支援SOCKS代理，需要安裝 PySocks
try:
    import socks
except ImportError:
    # 建立一個假的 socks 模組，避免程式在沒有安裝時出錯
    class FakeSocks:
        def set_default_proxy(self, *args, **kwargs):
            pass
    sys.modules['socks'] = FakeSocks()


# --- 後端處理核心 (程式碼不變) ---
class RealProxyAggregator:
    def __init__(self, status_callback=None):
        self.status_callback = status_callback
    def _log_status(self, message):
        if self.status_callback: self.status_callback(message)
    def fetch_and_parse_url(self, url, proxy_address=None):
        self._log_status(f"正在從 {url} 獲取內容...")
        headers = {'User-Agent': 'Clash/1.11.0'}
        proxies = {'http': proxy_address, 'https': proxy_address} if proxy_address else None
        
        if proxy_address:
             self._log_status(f"  (使用代理: {proxy_address})")
        else:
             self._log_status("  (未使用代理)")

        try:
            response = requests.get(url, headers=headers, proxies=proxies, timeout=15)
            response.raise_for_status()
            raw_content = response.text
            self._log_status("內容下載成功，正在嘗試解析...")
            try:
                decoded_content = base64.b64decode(raw_content.strip()).decode('utf-8')
                self._log_status("Base64 解碼成功。")
            except (base64.binascii.Error, UnicodeDecodeError):
                self._log_status("非 Base64 編碼，視為純文字格式。")
                decoded_content = raw_content
            nodes = [node.strip() for node in decoded_content.splitlines() if node.strip()]
            self._log_status(f"成功解析出 {len(nodes)} 個節點。")
            return nodes
        except requests.exceptions.ProxyError as e:
            self._log_status(f"錯誤：代理連線失敗。請檢查代理地址和狀態。\n原因: {e}")
            return[]
        except requests.exceptions.RequestException as e:
            self._log_status(f"錯誤：無法從 {url} 獲取內容。\n原因: {e}")
            return []
        except Exception as e:
            self._log_status(f"處理 {url} 時發生未知錯誤: {e}")
            return []
    def filter_and_sort_nodes(self, nodes):
        self._log_status("\n正在模擬節點延遲測試和篩選...")
        time.sleep(1)
        if not nodes:
            self._log_status("沒有節點可供篩選。")
            return []
        random.shuffle(nodes)
        num_to_keep = int(len(nodes) * 0.9)
        filtered_nodes = nodes[:num_to_keep]
        self._log_status(f"模擬測試完成。保留了 {len(filtered_nodes)} / {len(nodes)} 個節點。")
        return filtered_nodes
    def generate_final_subscription(self, nodes):
        self._log_status("\n正在生成最終的通用訂閱檔案...")
        if not nodes:
            self._log_status("沒有節點可供生成訂閱。")
            return ""
        full_content = "\n".join(nodes)
        encoded_content = base64.b64encode(full_content.encode('utf-8')).decode('utf-8')
        self._log_status("訂閱檔案生成完畢！")
        return encoded_content


# --- 圖形化介面 (GUI) ---

class AggregatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("代理聚合器 (v1.5 - 代理開關)")
        self.root.geometry("850x800")
        
        self.aggregator = RealProxyAggregator(status_callback=self.log_to_gui)
        
        main_frame = ttk.Frame(root, padding="10")
        main_frame.pack(fill="both", expand=True)

        main_frame.columnconfigure(0, weight=1)
        main_frame.rowconfigure(3, weight=1)
        main_frame.rowconfigure(4, weight=1)

        # 訂閱連結區
        sub_frame = ttk.LabelFrame(main_frame, text="訂閱連結 (一行一個)", padding="5")
        sub_frame.grid(row=0, column=0, sticky="ew", pady=(0, 5))
        self.sub_links_text = scrolledtext.ScrolledText(sub_frame, height=8, width=100, relief='solid', bd=1)
        self.sub_links_text.pack(fill='x', expand=True)

        # 代理設定區
        proxy_frame = ttk.LabelFrame(main_frame, text="代理伺服器", padding="5")
        proxy_frame.grid(row=1, column=0, sticky="ew", pady=5)
        
        proxy_header_frame = ttk.Frame(proxy_frame)
        proxy_header_frame.pack(fill='x', anchor='w')

        # **變更點**: 新增勾選框
        self.proxy_enabled = tk.BooleanVar(value=False) # 預設不勾選
        self.proxy_checkbox = ttk.Checkbutton(proxy_header_frame, text="啟用代理", variable=self.proxy_enabled)
        self.proxy_checkbox.pack(side='left', padx=(0, 10))

        ttk.Label(proxy_header_frame, text="地址 (例: http://127.0.0.1:7890):").pack(side='left')

        self.proxy_entry = ttk.Entry(proxy_frame)
        self.proxy_entry.pack(fill='x', pady=(5,0))
        self.proxy_entry.insert(tk.END, "http://127.0.0.1:10809")

        # 控制按鈕
        control_frame = ttk.Frame(main_frame)
        control_frame.grid(row=2, column=0, pady=10)
        self.run_button = ttk.Button(control_frame, text="執行處理", command=self.run_processing_thread)
        self.run_button.pack()

        # 日誌區
        log_frame = ttk.LabelFrame(main_frame, text="處理日誌", padding="5")
        log_frame.grid(row=3, column=0, sticky="nsew", pady=5)
        self.log_text = scrolledtext.ScrolledText(log_frame, state='disabled', wrap=tk.WORD)
        self.log_text.pack(fill='both', expand=True)
        
        # 結果區
        result_frame = ttk.LabelFrame(main_frame, text="結果", padding="5")
        result_frame.grid(row=4, column=0, sticky="nsew", pady=5)
        
        result_header = ttk.Frame(result_frame)
        result_header.pack(fill='x', anchor='n', pady=(0, 5))
        self.save_button = ttk.Button(result_header, text="儲存為檔案...", command=self.save_result_to_file)
        self.save_button.pack(side='right', anchor='ne')
        ttk.Label(result_header, text="通用訂閱格式 (Base64)").pack(side='left', anchor='nw')
        
        self.result_text = scrolledtext.ScrolledText(result_frame, wrap=tk.WORD)
        self.result_text.pack(fill='both', expand=True)

    def log_to_gui(self, message):
        self.root.after(0, self._append_log, message)

    def _append_log(self, message):
        self.log_text.config(state='normal')
        self.log_text.insert(tk.END, message + "\n")
        self.log_text.config(state='disabled')
        self.log_text.see(tk.END)

    def run_processing_thread(self):
        self.run_button.config(state='disabled')
        self.log_text.config(state='normal'); self.log_text.delete('1.0', tk.END); self.log_text.config(state='disabled')
        self.result_text.delete('1.0', tk.END)
        thread = threading.Thread(target=self.process_subscriptions, daemon=True)
        thread.start()

    def process_subscriptions(self):
        try:
            urls = self.sub_links_text.get('1.0', tk.END).strip().split('\n')
            urls = [url.strip() for url in urls if url.strip()]
            
            # **變更點**: 根據勾選框的狀態決定是否使用代理
            proxy_address = None
            if self.proxy_enabled.get():
                proxy_address = self.proxy_entry.get().strip()

            if not urls:
                self.log_to_gui("錯誤：請至少輸入一個訂閱連結。")
                return

            self.log_to_gui("=== 開始處理 ===")
            
            all_nodes = []
            for url in urls:
                nodes_from_url = self.aggregator.fetch_and_parse_url(url, proxy_address)
                all_nodes.extend(nodes_from_url)
            
            self.log_to_gui(f"\n總共獲取 {len(all_nodes)} 個初始節點。")
            
            filtered_nodes = self.aggregator.filter_and_sort_nodes(all_nodes)
            final_content = self.aggregator.generate_final_subscription(filtered_nodes)
            
            self.root.after(0, self.result_text.insert, '1.0', final_content)
            self.log_to_gui("\n=== 處理完成 ===")

        except Exception as e:
            self.log_to_gui(f"發生嚴重錯誤: {e}")
        finally:
            self.root.after(0, self.run_button.config, {'state': 'normal'})

    def save_result_to_file(self):
        content = self.result_text.get('1.0', tk.END).strip()
        if not content:
            messagebox.showwarning("內容為空", "沒有可以儲存的內容。")
            return
        file_path = filedialog.asksaveasfilename(title="儲存訂閱檔案", defaultextension=".txt", filetypes=[("文字檔案", "*.txt"), ("所有檔案", "*.*")])
        if not file_path:
            return
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