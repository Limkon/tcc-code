import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
from tkinterdnd2 import DND_FILES, TkinterDnD
import re
import threading
import queue
import time
from datetime import datetime
from icmplib import ping, exceptions
import socket
import csv

class NetToolApp(TkinterDnD.Tk):
    def __init__(self):
        super().__init__()
        self.title("多功能网络工具 (v3.2 批量工具版)")
        self.geometry("800x600") # 调整窗口大小

        self.task_queue = queue.Queue()
        self.stop_batch_event = threading.Event()
        
        self.create_widgets()
        self.create_context_menus()
        self.process_queue()
        
        self.protocol("WM_DELETE_WINDOW", self.on_closing)

    def create_widgets(self):
        # 不再需要选项卡，直接创建主框架
        main_frame = ttk.Frame(self, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # --- 输入区框架 ---
        input_frame = ttk.LabelFrame(main_frame, text="输入源与选项", padding="10")
        input_frame.pack(fill=tk.X, expand=False)
        
        self.input_method_var = tk.StringVar(value="file")
        ttk.Radiobutton(input_frame, text="从文件", variable=self.input_method_var, value="file", command=self.toggle_input_method).grid(row=0, column=0, sticky=tk.W, padx=5)
        ttk.Radiobutton(input_frame, text="从文本框粘贴", variable=self.input_method_var, value="text", command=self.toggle_input_method).grid(row=0, column=2, sticky=tk.W, padx=5)

        self.file_path_var = tk.StringVar()
        self.file_path_entry = ttk.Entry(input_frame, textvariable=self.file_path_var, width=50)
        self.file_path_entry.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        self.file_path_entry.drop_target_register(DND_FILES)
        self.file_path_entry.dnd_bind('<<Drop>>', self.handle_drop)
        self.browse_button = ttk.Button(input_frame, text="浏览...", command=self.browse_file)
        self.browse_button.grid(row=1, column=2, padx=5)
        
        self.text_input = scrolledtext.ScrolledText(input_frame, width=40, height=5, state=tk.DISABLED)
        self.text_input.grid(row=1, column=3, rowspan=4, sticky=(tk.W, tk.E, tk.N, tk.S), padx=10)
        
        ttk.Label(input_frame, text="Ping超时(ms):").grid(row=2, column=0, sticky=tk.W, pady=2)
        self.timeout_var = tk.StringVar(value="1000")
        self.timeout_entry = ttk.Entry(input_frame, textvariable=self.timeout_var, width=10)
        self.timeout_entry.grid(row=2, column=1, sticky=tk.W)

        ttk.Label(input_frame, text="Ping次数:").grid(row=3, column=0, sticky=tk.W, pady=2)
        self.count_var = tk.StringVar(value="10")
        self.count_entry = ttk.Entry(input_frame, textvariable=self.count_var, width=10)
        self.count_entry.grid(row=3, column=1, sticky=tk.W)
        
        ttk.Label(input_frame, text="扫描端口:").grid(row=4, column=0, sticky=tk.W, pady=2)
        self.ports_var = tk.StringVar(value="80,443,8080,1433,3306,3389")
        self.ports_entry = ttk.Entry(input_frame, textvariable=self.ports_var, width=20)
        self.ports_entry.grid(row=4, column=1, sticky=tk.W)
        input_frame.columnconfigure(1, weight=1)

        # --- 控制区框架 ---
        control_frame = ttk.Frame(main_frame)
        control_frame.pack(fill=tk.X, pady=10)
        self.ping_button = ttk.Button(control_frame, text="开始批量 Ping", command=self.start_ping_task)
        self.ping_button.pack(side=tk.LEFT, padx=10, ipady=5)
        self.scan_button = ttk.Button(control_frame, text="开始端口扫描", command=self.start_scan_task)
        self.scan_button.pack(side=tk.LEFT, padx=10, ipady=5)
        self.extract_button = ttk.Button(control_frame, text="从输入源提取IP", command=self.start_extract_task)
        self.extract_button.pack(side=tk.LEFT, padx=10, ipady=5)
        self.stop_button = ttk.Button(control_frame, text="停止任务", command=self.stop_batch_task, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=10, ipady=5)

        # --- 结果区框架 ---
        result_frame = ttk.LabelFrame(main_frame, text="结果显示", padding="10")
        result_frame.pack(fill=tk.BOTH, expand=True)
        
        self.tree = ttk.Treeview(result_frame, show='headings')
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb = ttk.Scrollbar(result_frame, orient="vertical", command=self.tree.yview)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.configure(yscrollcommand=vsb.set)
        
        # --- 进度条与状态栏 ---
        self.trough_color = '#F0F0F0'
        self.bar_color = '#0078D7'
        self.batch_progress_canvas = tk.Canvas(main_frame, height=18, background=self.trough_color, highlightthickness=0)
        self.batch_progress_canvas.pack(fill=tk.X, padx=10, pady=5)
        self.batch_progress_fill_id = self.batch_progress_canvas.create_rectangle(0, 0, 0, 20, fill=self.bar_color, outline="")
        self.batch_progress_text_id = self.batch_progress_canvas.create_text(0, 0, text="", anchor=tk.CENTER)
        
        self.export_button = ttk.Button(main_frame, text="将结果导出为CSV", command=self.export_to_csv)
        self.export_button.pack(pady=5, anchor=tk.E, padx=10)
        
        self.status_var = tk.StringVar(value="请选择输入方式并开始操作...")
        self.status_label = ttk.Label(main_frame, textvariable=self.status_var, anchor=tk.W)
        self.status_label.pack(fill=tk.X, padx=10, side=tk.BOTTOM)

    def create_context_menus(self):
        self.text_context_menu = tk.Menu(self, tearoff=0)
        self.text_context_menu.add_command(label="剪切", command=lambda: self.focus_get().event_generate("<<Cut>>"))
        self.text_context_menu.add_command(label="复制", command=lambda: self.focus_get().event_generate("<<Copy>>"))
        self.text_context_menu.add_command(label="粘贴", command=lambda: self.focus_get().event_generate("<<Paste>>"))
        self.text_context_menu.add_separator()
        self.text_context_menu.add_command(label="全选", command=self.select_all_text)
        text_widgets = [self.text_input, self.file_path_entry, self.timeout_entry, self.count_entry, self.ports_entry]
        for widget in text_widgets:
            widget.bind("<Button-3>", self.show_text_context_menu)
        
        self.tree_context_menu = tk.Menu(self, tearoff=0)
        self.tree_context_menu.add_command(label="复制选中行", command=self.copy_selected_tree_rows)
        self.tree_context_menu.add_separator()
        self.tree_context_menu.add_command(label="全选", command=lambda: self.tree.selection_add(self.tree.get_children()))
        self.tree.bind("<Button-3>", self.show_tree_context_menu)
        
    def show_text_context_menu(self, event):
        widget = event.widget
        try: has_selection = bool(widget.selection_get())
        except tk.TclError: has_selection = False
        state = tk.NORMAL if has_selection else tk.DISABLED
        self.text_context_menu.entryconfig("剪切", state=state)
        self.text_context_menu.entryconfig("复制", state=state)
        self.text_context_menu.tk_popup(event.x_root, event.y_root)

    def select_all_text(self):
        widget = self.focus_get()
        if isinstance(widget, (tk.Text, scrolledtext.ScrolledText)):
            widget.tag_add('sel', '1.0', 'end-1c'); widget.mark_set('insert', '1.0'); widget.see('insert')
        elif isinstance(widget, ttk.Entry): widget.selection_range(0, tk.END)
        return "break"

    def show_tree_context_menu(self, event):
        if self.tree.selection(): self.tree_context_menu.tk_popup(event.x_root, event.y_root)

    def copy_selected_tree_rows(self):
        text_to_copy = ["\t".join(self.tree['columns'])]
        for item_id in self.tree.selection():
            values = self.tree.item(item_id)['values']; text_to_copy.append("\t".join(map(str, values)))
        self.clipboard_clear(); self.clipboard_append("\n".join(text_to_copy))
    
    def on_closing(self):
        self.stop_batch_event.set()
        self.destroy()

    def update_batch_canvas_progress(self, percentage):
        canvas_width = self.batch_progress_canvas.winfo_width()
        canvas_height = self.batch_progress_canvas.winfo_height()
        if canvas_width > 1:
            fill_width = canvas_width * (percentage / 100.0)
            self.batch_progress_canvas.coords(self.batch_progress_fill_id, 0, 0, fill_width, canvas_height)
            text_color = 'white' if fill_width > (canvas_width / 2) + 15 else 'black'
            self.batch_progress_canvas.itemconfig(self.batch_progress_text_id, text=f"{int(percentage)}%", fill=text_color)
            self.batch_progress_canvas.coords(self.batch_progress_text_id, canvas_width / 2, canvas_height / 2)

    def process_queue(self):
        try:
            message = self.task_queue.get_nowait()
            msg_type, data = message

            if msg_type == 'batch_progress':
                progress_val, status_text = data
                self.update_batch_canvas_progress(progress_val)
                self.status_var.set(status_text)
            elif msg_type == 'treeview_setup': self.setup_treeview_columns(data)
            elif msg_type == 'treeview_row': self.tree.insert("", "end", values=data)
            elif msg_type == 'treeview_clear': self.clear_treeview()
            elif msg_type == 'complete':
                self.set_controls_state(tk.NORMAL)
                self.update_batch_canvas_progress(100)
                self.batch_progress_canvas.after(1000, lambda: (self.update_batch_canvas_progress(0), self.batch_progress_canvas.itemconfig(self.batch_progress_text_id, text="")))
                self.status_var.set(data)
                
        except queue.Empty: pass
        finally: self.after(100, self.process_queue)
    
    def toggle_input_method(self):
        state = tk.NORMAL if self.input_method_var.get() == "file" else tk.DISABLED
        self.file_path_entry.config(state=state); self.browse_button.config(state=state)
        self.text_input.config(state=tk.DISABLED if state == tk.NORMAL else tk.NORMAL)
    def handle_drop(self, event): self.file_path_var.set(event.data.strip('{}')); self.input_method_var.set('file'); self.toggle_input_method()
    def browse_file(self):
        filepath = filedialog.askopenfilename(filetypes=(("Text files", "*.txt"), ("All files", "*.*")))
        if filepath: self.file_path_var.set(filepath); self.input_method_var.set('file'); self.toggle_input_method()
    def get_input_content(self, get_hosts_list=True):
        method = self.input_method_var.get()
        content = ""
        if method == "file":
            filepath = self.file_path_var.get()
            if not filepath: messagebox.showerror("错误", "请先选择一个文件！"); return None
            try:
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f: content = f.read()
            except FileNotFoundError: messagebox.showerror("错误", f"文件未找到: {filepath}"); return None
        else: content = self.text_input.get("1.0", tk.END)
        if not content.strip(): messagebox.showinfo("提示", "输入源中没有有效内容。"); return None
        if get_hosts_list: return sorted(list(set(line.strip() for line in content.splitlines() if line.strip())))
        else: return content
    def set_controls_state(self, state, task_running=False):
        for widget in [self.ping_button, self.scan_button, self.extract_button, self.browse_button]: widget.config(state=state)
        self.stop_button.config(state=tk.NORMAL if task_running else tk.DISABLED)
    def setup_treeview_columns(self, columns):
        self.clear_treeview(); self.tree["columns"] = columns
        for col in columns:
            self.tree.heading(col, text=col, anchor=tk.CENTER, command=lambda _col=col: self.sort_treeview_column(_col, False))
            self.tree.column(col, anchor=tk.W, width=110, stretch=tk.YES)
    def sort_treeview_column(self, col, reverse):
        l = [(self.tree.set(k, col), k) for k in self.tree.get_children('')]
        try: l.sort(key=lambda t: float(t[0]), reverse=reverse)
        except (ValueError, TypeError): l.sort(key=lambda t: str(t[0]), reverse=reverse)
        for index, (val, k) in enumerate(l): self.tree.move(k, '', index)
        self.tree.heading(col, command=lambda: self.sort_treeview_column(col, not reverse))
    def clear_treeview(self):
        for item in self.tree.get_children(): self.tree.delete(item)
    def export_to_csv(self):
        if not self.tree.get_children(): messagebox.showinfo("提示", "结果表格中没有数据可导出。"); return
        filepath = filedialog.asksaveasfilename(defaultextension=".csv", filetypes=[("CSV files", "*.csv"), ("All files", "*.*")])
        if not filepath: return
        try:
            with open(filepath, 'w', newline='', encoding='utf-8-sig') as f:
                writer = csv.writer(f); writer.writerow(self.tree['columns'])
                for item in self.tree.get_children(): writer.writerow(self.tree.item(item)['values'])
            messagebox.showinfo("成功", f"结果已成功导出至:\n{filepath}")
        except Exception as e: messagebox.showerror("导出失败", f"发生错误: {e}")
    def start_ping_task(self):
        hosts = self.get_input_content(get_hosts_list=True)
        if not hosts: return
        self.set_controls_state(tk.DISABLED, task_running=True); self.stop_batch_event.clear()
        count = int(self.count_var.get()); timeout = int(self.timeout_var.get()) / 1000
        threading.Thread(target=self._run_ping_test, args=(hosts, count, timeout), daemon=True).start()
    def start_scan_task(self):
        hosts = self.get_input_content(get_hosts_list=True)
        if not hosts: return
        ports_str = self.ports_var.get()
        try: ports = [int(p.strip()) for p in ports_str.split(',') if p.strip()]; assert ports
        except (ValueError, AssertionError): messagebox.showerror("错误", "端口列表格式不正确。"); return
        self.set_controls_state(tk.DISABLED, task_running=True); self.stop_batch_event.clear()
        threading.Thread(target=self._run_port_scan, args=(hosts, ports), daemon=True).start()
    def start_extract_task(self):
        content = self.get_input_content(get_hosts_list=False)
        if not content: return
        self.set_controls_state(tk.DISABLED, task_running=True); self.stop_batch_event.clear()
        threading.Thread(target=self._run_extract_ips, args=(content,), daemon=True).start()
    def stop_batch_task(self):
        self.status_var.set("正在发送停止信号，请稍候..."); self.stop_button.config(state=tk.DISABLED)
        self.stop_batch_event.set()

    # --- Worker Threads ---
    def _run_ping_test(self, hosts, count, timeout):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("目标地址", "状态", "平均延迟(ms)", "丢包率(%)", "TTL")))
        total_pings = len(hosts) * count if count > 0 else len(hosts)
        pings_done = 0
        for i, host_addr in enumerate(hosts):
            if self.stop_batch_event.is_set(): break
            
            try:
                packets_success, total_latency, last_ttl = 0, 0, "N/A"
                for j in range(count):
                    if self.stop_batch_event.is_set(): break
                    
                    status_text = f"正在 Ping: {host_addr} ({i+1}/{len(hosts)})..."
                    
                    host = ping(host_addr, count=1, timeout=timeout, privileged=False)
                    
                    pings_done += 1
                    progress = pings_done * 100 / total_pings if total_pings > 0 else 0
                    self.task_queue.put(('batch_progress', (progress, status_text)))
                    
                    if host.is_alive:
                        packets_success += 1
                        total_latency += host.avg_rtt
                        last_ttl = getattr(host, 'ttl', "N/A")
                
                if self.stop_batch_event.is_set(): break
                status = "在线" if packets_success > 0 else "超时"
                avg_rtt = f"{total_latency / packets_success:.2f}" if packets_success > 0 else "N/A"
                loss = f"{(count - packets_success) * 100 / count:.0f}" if count > 0 else "100"
                self.task_queue.put(('treeview_row', (host_addr, status, avg_rtt, loss, last_ttl)))
            
            except (exceptions.NameLookupError, UnicodeError):
                pings_done += count
                self.task_queue.put(('treeview_row', (host_addr, "无效地址", "N/A", "100", "N/A")))
                progress = pings_done * 100 / total_pings if total_pings > 0 else 0
                self.task_queue.put(('batch_progress', (progress, f"跳过无效地址: {host_addr}")))
                continue
        
        completion_msg = f"任务已被用户停止。" if self.stop_batch_event.is_set() else f"Ping 测试完成。"
        self.task_queue.put(('complete', completion_msg))

    def _run_port_scan(self, hosts, ports):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("目标地址", "端口", "状态")))
        total_checks = len(hosts) * len(ports)
        checks_done = 0
        for i, host_addr in enumerate(hosts):
            if self.stop_batch_event.is_set(): break
            for port in ports:
                if self.stop_batch_event.is_set(): break
                
                checks_done += 1
                progress = checks_done * 100 / total_checks if total_checks > 0 else 0
                status_text = f"正在扫描: {host_addr}:{port} ({i+1}/{len(hosts)})"
                self.task_queue.put(('batch_progress', (progress, status_text)))
                
                try:
                    with socket.create_connection((host_addr, port), timeout=2) as sock: status = "开放 (Open)"
                except (socket.timeout, ConnectionRefusedError, OSError): status = "关闭 (Closed)"
                except (UnicodeError, socket.gaierror): status = "无效地址"
                self.task_queue.put(('treeview_row', (host_addr, port, status)))
            if self.stop_batch_event.is_set(): break
        completion_msg = f"任务已被用户停止。" if self.stop_batch_event.is_set() else f"端口扫描完成。"
        self.task_queue.put(('complete', completion_msg))
    
    def _run_extract_ips(self, content):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("提取到的IP地址",)))
        ip_pattern = r'\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}'
        found_ips = sorted(list(set(re.findall(ip_pattern, content))))
        for ip in found_ips:
            if self.stop_batch_event.is_set(): break
            self.task_queue.put(('treeview_row', (ip,)))
        completion_msg = f"任务已被用户停止。" if self.stop_batch_event.is_set() else f"提取完成，共找到 {len(found_ips)} 个唯一IP。"
        self.task_queue.put(('complete', completion_msg))


if __name__ == "__main__":
    app = NetToolApp()
    app.mainloop()