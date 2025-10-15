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
import platform

# 平台相关的导入，用于设置系统代理
try:
    if platform.system() == "Windows":
        import winreg
        import ctypes
except ImportError:
    # 允许程序在非Windows系统上运行，相关功能将不可用
    pass

class NetToolApp(TkinterDnD.Tk):
    def __init__(self):
        super().__init__()
        # 更新标题和窗口大小以适应新按钮
        self.title("多功能网络工具 (v3.7 系统代理)")
        self.geometry("900x720")

        self.task_queue = queue.Queue()
        self.stop_batch_event = threading.Event()
        # 新增：代理状态变量
        self.is_proxy_set = False
        self.proxy_settings_to_restore = {}
        
        self.original_data = []  # 用于存储原始结果数据，便于过滤
        
        self.create_widgets()
        self.create_context_menus()
        self.process_queue()
        
        self.protocol("WM_DELETE_WINDOW", self.on_closing)

    def create_widgets(self):
        main_frame = ttk.Frame(self, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # --- 底部状态栏与导出按钮框架 (修改: 优先打包到底部) ---
        bottom_frame = ttk.Frame(main_frame)
        bottom_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=(5, 0))

        self.status_var = tk.StringVar(value="请选择输入方式并开始操作...")
        self.status_label = ttk.Label(bottom_frame, textvariable=self.status_var, anchor=tk.W, wraplength=800)
        self.status_label.pack(side=tk.LEFT, fill=tk.X, expand=True)

        self.export_button = ttk.Button(bottom_frame, text="将结果导出为CSV", command=self.export_to_csv)
        self.export_button.pack(side=tk.RIGHT, padx=(10, 0))
        
        # --- 进度条 (修改: 打包到底部, 位于状态栏之上) ---
        self.trough_color = '#F0F0F0'
        self.bar_color = '#0078D7'
        self.bar_color_complete = '#28A745' # Green color for completion
        self.batch_progress_canvas = tk.Canvas(main_frame, height=18, background=self.trough_color, highlightthickness=0)
        self.batch_progress_canvas.pack(side=tk.BOTTOM, fill=tk.X, pady=5)
        self.batch_progress_fill_id = self.batch_progress_canvas.create_rectangle(0, 0, 0, 20, fill=self.bar_color, outline="")
        self.batch_progress_text_id = self.batch_progress_canvas.create_text(0, 0, text="", anchor=tk.CENTER)

        # --- 输入区框架 (批量任务) (修改: 打包到顶部) ---
        input_frame = ttk.LabelFrame(main_frame, text="批量任务: 输入源与选项", padding="10")
        input_frame.pack(side=tk.TOP, fill=tk.X, expand=False, pady=(0, 5))
        
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
        self.count_var = tk.StringVar(value="5")
        self.count_entry = ttk.Entry(input_frame, textvariable=self.count_var, width=10)
        self.count_entry.grid(row=3, column=1, sticky=tk.W)
        
        ttk.Label(input_frame, text="批量扫描端口:").grid(row=4, column=0, sticky=tk.W, pady=2)
        self.ports_var = tk.StringVar(value="80,443,8080,1433,3306,3389")
        self.ports_entry = ttk.Entry(input_frame, textvariable=self.ports_var, width=20)
        self.ports_entry.grid(row=4, column=1, sticky=tk.W)
        input_frame.columnconfigure(3, weight=1)

        # --- 单个目标扫描区 (修改: 打包到顶部) ---
        single_scan_frame = ttk.LabelFrame(main_frame, text="单个目标端口扫描", padding="10")
        single_scan_frame.pack(side=tk.TOP, fill=tk.X, pady=5)

        ttk.Label(single_scan_frame, text="目标IP或域名:").grid(row=0, column=0, sticky=tk.W, padx=5)
        self.single_ip_var = tk.StringVar(value="127.0.0.1")
        self.single_ip_entry = ttk.Entry(single_scan_frame, textvariable=self.single_ip_var, width=25)
        self.single_ip_entry.grid(row=0, column=1, sticky=tk.W)

        ttk.Label(single_scan_frame, text="端口范围 (如 1-1024,3306):").grid(row=0, column=2, sticky=tk.W, padx=(15, 5))
        self.single_ports_var = tk.StringVar(value="1-1024, 3306, 8080-8090")
        self.single_ports_entry = ttk.Entry(single_scan_frame, textvariable=self.single_ports_var, width=35)
        self.single_ports_entry.grid(row=0, column=3, sticky=(tk.W, tk.E))
        
        self.single_scan_button = ttk.Button(single_scan_frame, text="扫描指定目标", command=self.start_single_scan_task)
        self.single_scan_button.grid(row=0, column=4, padx=10, ipady=2)
        single_scan_frame.columnconfigure(3, weight=1)

        # --- 控制区框架 (修改: 打包到顶部) ---
        control_frame = ttk.LabelFrame(main_frame, text="批量任务控制", padding="10")
        control_frame.pack(side=tk.TOP, fill=tk.X, pady=5)
        self.ping_button = ttk.Button(control_frame, text="开始批量 Ping", command=self.start_ping_task)
        self.ping_button.pack(side=tk.LEFT, padx=10, ipady=5)
        self.scan_button = ttk.Button(control_frame, text="开始批量端口扫描", command=self.start_scan_task)
        self.scan_button.pack(side=tk.LEFT, padx=10, ipady=5)
        self.extract_button = ttk.Button(control_frame, text="从输入源提取IP", command=self.start_extract_task)
        self.extract_button.pack(side=tk.LEFT, padx=10, ipady=5)
        
        self.proxy_button = ttk.Button(control_frame, text="设置系统代理", command=self.toggle_system_proxy)
        self.proxy_button.pack(side=tk.LEFT, padx=5, ipady=5)

        self.stop_button = ttk.Button(control_frame, text="停止当前任务", command=self.stop_batch_task, state=tk.DISABLED)
        self.stop_button.pack(side=tk.RIGHT, padx=10, ipady=5)

        # --- 结果区框架 (修改: 使用grid布局并实现滚动条自动隐藏) ---
        result_frame = ttk.LabelFrame(main_frame, text="结果显示", padding="10")
        result_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # 配置 result_frame 的网格布局权重，让包含Treeview的行(1)和列(0)可以伸缩
        result_frame.grid_rowconfigure(1, weight=1)
        result_frame.grid_columnconfigure(0, weight=1)

        search_frame = ttk.Frame(result_frame)
        search_frame.grid(row=0, column=0, columnspan=2, sticky=tk.EW, pady=(0, 5))
        ttk.Label(search_frame, text="过滤结果:").pack(side=tk.LEFT, padx=5)
        self.search_var = tk.StringVar()
        self.search_entry = ttk.Entry(search_frame, textvariable=self.search_var, width=30)
        self.search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.search_entry.bind("<KeyRelease>", self.filter_treeview)
        
        self.tree = ttk.Treeview(result_frame, show='headings')

        # --- 创建并配置滚动条 ---
        self.vsb = ttk.Scrollbar(result_frame, orient="vertical", command=self.tree.yview)
        self.hsb = ttk.Scrollbar(result_frame, orient="horizontal", command=self.tree.xview)
        # 将Treeview的滚动命令连接到自定义的包装器方法
        self.tree.configure(yscrollcommand=self._yscroll_set_wrapper, xscrollcommand=self._xscroll_set_wrapper)
        
        # --- 使用 grid 布局结果区控件 ---
        self.tree.grid(row=1, column=0, sticky='nsew')
        self.vsb.grid(row=1, column=1, sticky='ns')
        self.hsb.grid(row=2, column=0, sticky='ew')
        
    def _yscroll_set_wrapper(self, *args):
        """包装垂直滚动条的set命令以实现自动隐藏。"""
        # 始终调用原始的set方法
        self.vsb.set(*args)
        # 根据滚动条的位置判断是否需要显示
        if float(args[0]) <= 0 and float(args[1]) >= 1:
            self.vsb.grid_remove() # 如果内容完全可见，则隐藏滚动条
        else:
            self.vsb.grid() # 否则显示滚动条

    def _xscroll_set_wrapper(self, *args):
        """包装水平滚动条的set命令以实现自动隐藏。"""
        # 始终调用原始的set方法
        self.hsb.set(*args)
        # 根据滚动条的位置判断是否需要显示
        if float(args[0]) <= 0 and float(args[1]) >= 1:
            self.hsb.grid_remove() # 如果内容完全可见，则隐藏滚动条
        else:
            self.hsb.grid() # 否则显示滚动条

    def create_context_menus(self):
        self.text_context_menu = tk.Menu(self, tearoff=0)
        self.text_context_menu.add_command(label="剪切", command=lambda: self.focus_get().event_generate("<<Cut>>"))
        self.text_context_menu.add_command(label="复制", command=lambda: self.focus_get().event_generate("<<Copy>>"))
        self.text_context_menu.add_command(label="粘贴", command=lambda: self.focus_get().event_generate("<<Paste>>"))
        self.text_context_menu.add_separator()
        self.text_context_menu.add_command(label="全选", command=self.select_all_text)
        text_widgets = [self.text_input, self.file_path_entry, self.timeout_entry, 
                        self.count_entry, self.ports_entry, self.single_ip_entry, 
                        self.single_ports_entry]
        for widget in text_widgets:
            widget.bind("<Button-3>", self.show_text_context_menu)
        
        self.tree_context_menu = tk.Menu(self, tearoff=0)
        self.tree_context_menu.add_command(label="复制选中行", command=self.copy_selected_tree_rows)
        self.tree_context_menu.add_command(label="去除不在线IP", command=self.remove_offline_ips)
        self.tree_context_menu.add_command(label="去除重复IP", command=self.remove_duplicate_ips)
        self.tree_context_menu.add_separator()
        self.tree_context_menu.add_command(label="删除选中行", command=self.delete_selected_tree_rows)
        self.tree_context_menu.add_separator()
        self.tree_context_menu.add_command(label="全选", command=lambda: self.tree.selection_add(self.tree.get_children()))
        self.tree.bind("<Button-3>", self.show_tree_context_menu)
        self.tree.bind("<Control-a>", lambda e: self.tree.selection_add(self.tree.get_children()))
        
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
        columns = self.tree['columns']
        is_extract_ip = columns == ("提取到的IP地址",)
        text_to_copy = []
        if not is_extract_ip:
            text_to_copy.append("\t".join(columns))
        for item_id in self.tree.selection():
            values = self.tree.item(item_id)['values']
            text_to_copy.append("\t".join(map(str, values)))
        self.clipboard_clear()
        self.clipboard_append("\n".join(text_to_copy))
    
    def remove_offline_ips(self):
        columns = self.tree['columns']
        if "状态" not in columns:
            messagebox.showinfo("提示", "当前结果不包含'状态'列，无法去除不在线IP。")
            return
        status_index = columns.index("状态")
        items_to_delete = []
        for item_id in self.tree.get_children():
            values = self.tree.item(item_id)['values']
            status = values[status_index]
            if status in ("超时", "无效地址", "错误"):
                items_to_delete.append(item_id)
        for item_id in items_to_delete:
            self.tree.delete(item_id)
        self.status_var.set(f"已去除 {len(items_to_delete)} 个不在线IP。")
        # 更新原始数据
        self.original_data = [self.tree.item(item)['values'] for item in self.tree.get_children()]
    
    def remove_duplicate_ips(self):
        columns = self.tree['columns']
        ip_col_names = {"目标地址", "提取到的IP地址"}
        ip_index = None
        for i, col in enumerate(columns):
            if col in ip_col_names:
                ip_index = i
                break
        if ip_index is None:
            messagebox.showinfo("提示", "当前结果不包含IP地址列，无法去除重复IP。")
            return
        
        seen_ips = set()
        items_to_delete = []
        for item_id in self.tree.get_children():
            values = self.tree.item(item_id)['values']
            ip = values[ip_index]
            if ip in seen_ips:
                items_to_delete.append(item_id)
            else:
                seen_ips.add(ip)
        
        for item_id in items_to_delete:
            self.tree.delete(item_id)
        
        self.status_var.set(f"已去除 {len(items_to_delete)} 个重复IP。")
        # 更新原始数据
        self.original_data = [self.tree.item(item)['values'] for item in self.tree.get_children()]

    def delete_selected_tree_rows(self):
        """新增：删除Treeview中选中的行"""
        selected_items = self.tree.selection()
        if not selected_items:
            return
        
        # 弹出确认对话框
        if messagebox.askyesno("确认删除", f"确定要删除选中的 {len(selected_items)} 行吗？"):
            for item_id in selected_items:
                self.tree.delete(item_id)
            
            # 更新原始数据以保持过滤功能同步
            self.original_data = [self.tree.item(item)['values'] for item in self.tree.get_children()]
            self.status_var.set(f"已删除 {len(selected_items)} 行。")

    def filter_treeview(self, event=None):
        query = self.search_var.get().lower()
        if not query:
            # 恢复所有行
            self.clear_treeview()
            for values in self.original_data:
                self.tree.insert("", "end", values=values)
            return
        
        matching_items = []
        for values in self.original_data:
            if any(query in str(val).lower() for val in values):
                matching_items.append(values)
        
        self.clear_treeview()
        for values in matching_items:
            self.tree.insert("", "end", values=values)
    
    def on_closing(self):
        # 修改：退出时检查并提示恢复代理设置
        if self.is_proxy_set:
            if messagebox.askyesno("退出前确认", "检测到程序设置了系统代理，是否在退出前自动取消代理设置？"):
                self._unset_system_proxy()
        self.stop_batch_event.set()
        self.destroy()

    def update_batch_canvas_progress(self, percentage):
        self.after(0, self._update_canvas, percentage)

    def _update_canvas(self, percentage):
        canvas_width = self.batch_progress_canvas.winfo_width()
        canvas_height = self.batch_progress_canvas.winfo_height()
        if canvas_width > 1:
            fill_width = canvas_width * (percentage / 100.0)
            self.batch_progress_canvas.coords(self.batch_progress_fill_id, 0, 0, fill_width, canvas_height)
            text_color = 'white' if fill_width > (canvas_width / 2) + 20 else 'black'
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
            elif msg_type == 'treeview_setup':
                self.original_data = []  # 重置原始数据
                self.setup_treeview_columns(data)
            elif msg_type == 'treeview_row':
                item_id = self.tree.insert("", "end", values=data)
                self.original_data.append(data)
            elif msg_type == 'treeview_clear': self.clear_treeview()
            elif msg_type == 'complete':
                self.set_controls_state(tk.NORMAL)
                self.update_batch_canvas_progress(100)
                self.status_var.set(data)
                
                self.batch_progress_canvas.itemconfig(self.batch_progress_fill_id, fill=self.bar_color_complete)
                canvas_width = self.batch_progress_canvas.winfo_width()
                canvas_height = self.batch_progress_canvas.winfo_height()
                if canvas_width > 1:
                    self.batch_progress_canvas.itemconfig(self.batch_progress_text_id, text="完成")
                    self.batch_progress_canvas.coords(self.batch_progress_text_id, canvas_width / 2, canvas_height / 2)

        except queue.Empty: pass
        finally: self.after(100, self.process_queue)
    
    def toggle_input_method(self):
        is_file_method = self.input_method_var.get() == "file"
        file_state = tk.NORMAL if is_file_method else tk.DISABLED
        text_state = tk.DISABLED if is_file_method else tk.NORMAL
        
        self.file_path_entry.config(state=file_state)
        self.browse_button.config(state=file_state)
        self.text_input.config(state=text_state)
        
        if is_file_method:
             self.status_var.set("请通过拖拽或浏览选择文件，或切换到文本框粘贴模式。")
        else:
             self.status_var.set("请在右侧文本框粘贴内容。")

    def handle_drop(self, event): 
        filepath = event.data.strip('{}')
        self.file_path_var.set(filepath)
        self.input_method_var.set('file')
        self.toggle_input_method()

    def browse_file(self):
        filepath = filedialog.askopenfilename(filetypes=(("Text files", "*.txt"), ("All files", "*.*")))
        if filepath: 
            self.file_path_var.set(filepath)
            self.input_method_var.set('file')
            self.toggle_input_method()

    def get_input_content(self, get_hosts_list=True):
        method = self.input_method_var.get()
        content = ""
        if method == "file":
            filepath = self.file_path_var.get()
            if not filepath: 
                messagebox.showerror("错误", "请先选择一个文件！")
                return None
            try:
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f: content = f.read()
            except FileNotFoundError: 
                messagebox.showerror("错误", f"文件未找到: {filepath}")
                return None
        else: content = self.text_input.get("1.0", tk.END)
        
        if not content.strip(): 
            messagebox.showinfo("提示", "输入源中没有有效内容。")
            return None
            
        if get_hosts_list: 
            hosts = re.findall(r'[a-zA-Z0-9\-\.]+\.[a-zA-Z]{2,}', content)
            ips = re.findall(r'\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}', content)
            all_found = hosts + ips
            return sorted(list(set(line.strip() for line in all_found if line.strip())))
        else: 
            return content

    def set_controls_state(self, state, task_running=False):
        # 修改：将新按钮加入状态管理
        start_buttons = [self.ping_button, self.scan_button, 
                         self.extract_button, self.browse_button, 
                         self.single_scan_button, self.proxy_button]
        for widget in start_buttons:
            widget.config(state=state)
        
        self.stop_button.config(state=tk.NORMAL if task_running else tk.DISABLED)

    def setup_treeview_columns(self, columns):
        self.clear_treeview()
        self.tree["columns"] = columns
        for col in columns:
            self.tree.heading(col, text=col, anchor=tk.CENTER, command=lambda _col=col: self.sort_treeview_column(_col, False))
            self.tree.column(col, anchor=tk.W, width=110, stretch=tk.YES)

    def sort_treeview_column(self, col, reverse):
        def try_float(val):
            try: return float(val)
            except (ValueError, TypeError): return str(val)

        l = [(self.tree.set(k, col), k) for k in self.tree.get_children('')]
        l.sort(key=lambda t: try_float(t[0]), reverse=reverse)
        
        for index, (val, k) in enumerate(l): 
            self.tree.move(k, '', index)
        self.tree.heading(col, command=lambda: self.sort_treeview_column(col, not reverse))

    def clear_treeview(self):
        for item in self.tree.get_children(): self.tree.delete(item)

    def export_to_csv(self):
        if not self.tree.get_children(): 
            messagebox.showinfo("提示", "结果表格中没有数据可导出。")
            return
            
        filepath = filedialog.asksaveasfilename(
            defaultextension=".csv", 
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
            title="将结果导出为CSV文件"
        )
        if not filepath: return
        
        try:
            with open(filepath, 'w', newline='', encoding='utf-8-sig') as f:
                writer = csv.writer(f)
                writer.writerow(self.tree['columns'])
                for item in self.tree.get_children(): 
                    writer.writerow(self.tree.item(item)['values'])
            messagebox.showinfo("成功", f"结果已成功导出至:\n{filepath}")
        except Exception as e: 
            messagebox.showerror("导出失败", f"发生错误: {e}")

    def reset_progress_bar(self):
        """Resets the progress bar to its default state before a task starts."""
        self.batch_progress_canvas.itemconfig(self.batch_progress_fill_id, fill=self.bar_color)
        self.update_batch_canvas_progress(0)
        self.batch_progress_canvas.itemconfig(self.batch_progress_text_id, text="")

    def start_ping_task(self):
        self.reset_progress_bar()
        hosts = self.get_input_content(get_hosts_list=True)
        if not hosts: return
        self.set_controls_state(tk.DISABLED, task_running=True)
        self.stop_batch_event.clear()
        try:
            count = int(self.count_var.get())
            timeout = int(self.timeout_var.get()) / 1000
            if count <= 0 or timeout <= 0: raise ValueError
        except ValueError:
            messagebox.showerror("输入错误", "Ping次数和超时必须是正整数。")
            self.set_controls_state(tk.NORMAL)
            return
        threading.Thread(target=self._run_ping_test, args=(hosts, count, timeout), daemon=True).start()

    def start_scan_task(self):
        self.reset_progress_bar()
        hosts = self.get_input_content(get_hosts_list=True)
        if not hosts: return
        ports_str = self.ports_var.get()
        try: 
            ports = [int(p.strip()) for p in ports_str.split(',') if p.strip()]
            if not ports: raise ValueError
        except (ValueError, AssertionError): 
            messagebox.showerror("错误", "批量扫描的端口列表格式不正确。请使用逗号分隔的数字。")
            return
        self.set_controls_state(tk.DISABLED, task_running=True)
        self.stop_batch_event.clear()
        threading.Thread(target=self._run_port_scan, args=(hosts, ports), daemon=True).start()

    def start_extract_task(self):
        self.reset_progress_bar()
        content = self.get_input_content(get_hosts_list=False)
        if not content: return
        self.set_controls_state(tk.DISABLED, task_running=True)
        self.stop_batch_event.clear()
        threading.Thread(target=self._run_extract_ips, args=(content,), daemon=True).start()

    def start_single_scan_task(self):
        self.reset_progress_bar()
        host = self.single_ip_var.get().strip()
        if not host:
            messagebox.showerror("错误", "请输入一个有效的目标IP或域名。")
            return

        ports = self._parse_port_range(self.single_ports_var.get())
        if ports is None: return
        if not ports:
            messagebox.showerror("错误", "请输入要扫描的端口。")
            return

        self.set_controls_state(tk.DISABLED, task_running=True)
        self.stop_batch_event.clear()
        threading.Thread(target=self._run_single_ip_port_scan, args=(host, ports), daemon=True).start()

    def stop_batch_task(self):
        self.status_var.set("正在发送停止信号，请稍候...")
        self.stop_button.config(state=tk.DISABLED)
        self.stop_batch_event.set()

    def _parse_port_range(self, port_str):
        ports = set()
        if not port_str: return []
        try:
            parts = port_str.split(',')
            for part in parts:
                part = part.strip()
                if not part: continue
                if '-' in part:
                    start, end = map(int, part.split('-'))
                    if not (0 < start <= end < 65536): raise ValueError
                    ports.update(range(start, end + 1))
                else:
                    port_num = int(part)
                    if not (0 < port_num < 65536): raise ValueError
                    ports.add(port_num)
            return sorted(list(ports))
        except ValueError:
            messagebox.showerror("端口格式错误", f"无效的端口或范围: '{part}'。\n请确保端口在1-65535之间，且范围格式为 '起始-结束'。")
            return None

    # --- 系统代理相关方法 ---
    def toggle_system_proxy(self):
        """切换系统代理的设置或取消状态。"""
        if platform.system() != "Windows":
            messagebox.showerror("功能限制", "此功能目前仅支持Windows操作系统。")
            return

        if self.is_proxy_set:
            # 如果代理已设置，则调用取消方法
            self._unset_system_proxy()
        else:
            # 如果代理未设置，则从Treeview中获取信息进行设置
            selected_items = self.tree.selection()
            if len(selected_items) != 1:
                messagebox.showerror("选择错误", "请在结果列表中【包含IP和端口列】的结果里，精确选择一个目标作为代理服务器。")
                return

            item = self.tree.item(selected_items[0])
            values = item['values']
            columns = self.tree['columns']

            try:
                ip_index, port_index = -1, -1
                ip_col_names = ("目标地址",)
                port_col_names = ("端口", "开放端口")

                for i, col_name in enumerate(columns):
                    if col_name in ip_col_names: ip_index = i
                    elif col_name in port_col_names: port_index = i

                if ip_index == -1 or port_index == -1:
                    messagebox.showerror("数据错误", "选中的行没有有效的'目标地址'和'端口'列。\n请先执行端口扫描任务。")
                    return

                proxy_ip = str(values[ip_index])
                proxy_port = int(values[port_index])

                if not (0 < proxy_port < 65536): raise ValueError("端口号无效")
                
                self._set_system_proxy(proxy_ip, proxy_port)

            except (ValueError, IndexError) as e:
                messagebox.showerror("数据格式错误", f"选中的行数据无法解析为有效的IP和端口号。\n错误: {e}")
                return

    def _set_system_proxy(self, ip, port):
        """(仅Windows) 设置系统代理。"""
        try:
            key_path = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
            # 以读写权限打开注册表项
            internet_settings_key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_READ | winreg.KEY_WRITE)

            # 备份当前设置
            try:
                self.proxy_settings_to_restore['ProxyEnable'], _ = winreg.QueryValueEx(internet_settings_key, "ProxyEnable")
            except FileNotFoundError:
                self.proxy_settings_to_restore['ProxyEnable'] = 0 # 默认未启用
            try:
                self.proxy_settings_to_restore['ProxyServer'], _ = winreg.QueryValueEx(internet_settings_key, "ProxyServer")
            except FileNotFoundError:
                self.proxy_settings_to_restore['ProxyServer'] = "" # 默认无服务器地址

            # 应用新设置
            proxy_address = f"{ip}:{port}"
            winreg.SetValueEx(internet_settings_key, "ProxyEnable", 0, winreg.REG_DWORD, 1)
            winreg.SetValueEx(internet_settings_key, "ProxyServer", 0, winreg.REG_SZ, proxy_address)
            winreg.CloseKey(internet_settings_key)

            # 通知系统设置已更改，使其立即生效
            ctypes.windll.Wininet.InternetSetOptionW(0, 39, 0, 0) # INTERNET_OPTION_PROXY
            ctypes.windll.Wininet.InternetSetOptionW(0, 37, 0, 0) # INTERNET_OPTION_REFRESH

            # 更新UI状态
            self.is_proxy_set = True
            self.proxy_button.config(text="取消系统代理")
            self.status_var.set(f"系统代理已设置为: {proxy_address}")
            messagebox.showinfo("成功", f"系统代理已成功设置为 {proxy_address}")

        except Exception as e:
            messagebox.showerror("设置代理失败", f"无法修改注册表: {e}\n请尝试以管理员权限运行本程序。")

    def _unset_system_proxy(self):
        """(仅Windows) 取消系统代理，恢复到之前的设置。"""
        try:
            key_path = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
            internet_settings_key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_WRITE)

            # 恢复之前备份的设置
            original_enable = self.proxy_settings_to_restore.get('ProxyEnable', 0)
            original_server = self.proxy_settings_to_restore.get('ProxyServer', "")
            
            winreg.SetValueEx(internet_settings_key, "ProxyEnable", 0, winreg.REG_DWORD, original_enable)
            winreg.SetValueEx(internet_settings_key, "ProxyServer", 0, winreg.REG_SZ, original_server)
            winreg.CloseKey(internet_settings_key)

            # 通知系统设置已更改
            ctypes.windll.Wininet.InternetSetOptionW(0, 39, 0, 0) # INTERNET_OPTION_PROXY
            ctypes.windll.Wininet.InternetSetOptionW(0, 37, 0, 0) # INTERNET_OPTION_REFRESH

            # 更新UI状态
            self.is_proxy_set = False
            self.proxy_button.config(text="设置系统代理")
            self.status_var.set("系统代理已恢复。")
            self.proxy_settings_to_restore = {} # 清空备份
            messagebox.showinfo("成功", "系统代理已恢复至之前设置。")

        except Exception as e:
            messagebox.showerror("取消代理失败", f"无法修改注册表: {e}\n请尝试以管理员权限运行本程序。")

    # --- Worker Threads ---
    def _run_ping_test(self, hosts, count, timeout):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("目标地址", "状态", "平均延迟(ms)", "丢包率(%)", "TTL")))
        total_hosts = len(hosts)
        
        for i, host_addr in enumerate(hosts):
            if self.stop_batch_event.is_set(): break
            
            progress = (i + 1) * 100 / total_hosts
            status_text = f"正在 Ping ({i+1}/{total_hosts}): {host_addr}..."
            self.task_queue.put(('batch_progress', (progress, status_text)))
            
            try:
                host_result = ping(host_addr, count=count, timeout=timeout, privileged=False)
                
                if host_result.is_alive:
                    status = "在线"
                    avg_rtt = f"{host_result.avg_rtt:.2f}"
                    loss = f"{host_result.packet_loss * 100:.0f}"
                    last_ttl = "N/A"
                else:
                    status = "超时"
                    avg_rtt = "N/A"
                    loss = "100"
                    last_ttl = "N/A"

                self.task_queue.put(('treeview_row', (host_addr, status, avg_rtt, loss, last_ttl)))

            except (exceptions.NameLookupError, UnicodeError):
                self.task_queue.put(('treeview_row', (host_addr, "无效地址", "N/A", "100", "N/A")))
            except Exception as e:
                 self.task_queue.put(('treeview_row', (host_addr, f"错误", "N/A", "100", "N/A")))
        
        completion_msg = "任务已被用户停止。" if self.stop_batch_event.is_set() else f"批量Ping测试完成。"
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
                status_text = f"批量扫描 ({checks_done}/{total_checks}): {host_addr}:{port}"
                self.task_queue.put(('batch_progress', (progress, status_text)))
                
                status = ""
                try:
                    with socket.create_connection((host_addr, port), timeout=2):
                        status = "开放 (Open)"
                except socket.timeout: status = "超时 (Timeout)"
                except ConnectionRefusedError: status = "关闭 (Closed)"
                except OSError: status = "拒绝 (Refused)"
                except (UnicodeError, socket.gaierror):
                    status = "无效地址"
                    break
                
                if status == "开放 (Open)":
                    self.task_queue.put(('treeview_row', (host_addr, port, status)))

            if status == "无效地址":
                 self.task_queue.put(('treeview_row', (host_addr, "*", status)))


        completion_msg = "任务已被用户停止。" if self.stop_batch_event.is_set() else f"批量端口扫描完成。"
        self.task_queue.put(('complete', completion_msg))
    
    def _run_single_ip_port_scan(self, host, ports):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("目标地址", "开放端口", "服务/备注")))
        total_checks = len(ports)
        checks_done = 0
        open_ports_found = 0

        for port in ports:
            if self.stop_batch_event.is_set(): break

            checks_done += 1
            progress = checks_done * 100 / total_checks if total_checks > 0 else 0
            status_text = f"正在扫描 {host}:{port} ({checks_done}/{total_checks})"
            self.task_queue.put(('batch_progress', (progress, status_text)))

            try:
                with socket.create_connection((host, port), timeout=1):
                    service = "未知"
                    try: service = socket.getservbyport(port)
                    except (OSError, TypeError): pass
                    self.task_queue.put(('treeview_row', (host, port, service)))
                    open_ports_found += 1
            except (socket.timeout, ConnectionRefusedError, OSError):
                pass
            except (UnicodeError, socket.gaierror) as e:
                self.task_queue.put(('complete', f"错误: 无法解析主机 '{host}'"))
                return

        if self.stop_batch_event.is_set():
            completion_msg = f"任务已被用户停止。为 {host} 找到 {open_ports_found} 个开放端口。"
        else:
            completion_msg = f"对 {host} 的扫描完成。共找到 {open_ports_found} 个开放端口。"
        self.task_queue.put(('complete', completion_msg))

    def _run_extract_ips(self, content):
        self.task_queue.put(('treeview_clear', None))
        self.task_queue.put(('treeview_setup', ("提取到的IP地址",)))
        
        ip_pattern = r'\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}'
        candidates = re.findall(ip_pattern, content)
        
        valid_ips = []
        for ip in candidates:
            try:
                if all(0 <= int(octet) <= 255 for octet in ip.split('.')):
                    valid_ips.append(ip)
            except (ValueError, IndexError):
                continue
        
        found_ips = sorted(list(set(valid_ips)))
        
        total_found = len(found_ips)
        if total_found == 0:
            self.task_queue.put(('complete', "未在文本中找到有效的IP地址。"))
            return

        for i, ip in enumerate(found_ips):
            if self.stop_batch_event.is_set(): break
            progress = (i + 1) * 100 / total_found
            self.task_queue.put(('batch_progress', (progress, f"正在提取IP... ({i+1}/{total_found})")))
            self.task_queue.put(('treeview_row', (ip,)))
            time.sleep(0.01)

        completion_msg = f"任务已被用户停止。" if self.stop_batch_event.is_set() else f"提取完成，共找到 {total_found} 个唯一IP。"
        self.task_queue.put(('complete', completion_msg))


if __name__ == "__main__":
    app = NetToolApp()
    app.mainloop()