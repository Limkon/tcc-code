import os
import zipfile
import shutil
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext
from opencc import OpenCC
from datetime import datetime
import subprocess
import sys

# 支持的文本文件扩展名
text_extensions = ('.html', '.js', '.json', '.css', '.txt', '.md', '.py', '.ts', '.vue', '.php')

# 统一的日志输出函数
def log(message, log_callback):
    timestamp = datetime.now().strftime('%H:%M:%S')
    log_callback(f"[{timestamp}] {message}")

# 转换文本文件
def convert_file(filepath, cc, log_callback):
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
        converted = cc.convert(content)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(converted)
        log(f"[✓] 已转换：{filepath}", log_callback)
    except Exception as e:
        log(f"[×] 跳过：{filepath}，错误：{e}", log_callback)

# 递归处理目录
def process_directory(root_dir, cc, log_callback):
    for root, _, files in os.walk(root_dir):
        for file in files:
            if file.lower().endswith(text_extensions):
                convert_file(os.path.join(root, file), cc, log_callback)

# 转换 ZIP 文件
def convert_zip_file(zip_path, direction, log_callback):
    extract_dir = 'temp_gui_extract'
    convert_dir = 'temp_gui_convert'
    out_suffix = 'simplified' if direction == 't2s' else 'traditional'
    output_zip = os.path.splitext(zip_path)[0] + f'-{out_suffix}.zip'

    for path in [extract_dir, convert_dir]:
        if os.path.exists(path):
            shutil.rmtree(path)

    log("→ 解压 ZIP 文件...", log_callback)
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(extract_dir)

    shutil.copytree(extract_dir, convert_dir)

    cc = OpenCC(direction)
    log(f"→ 转换为 {'简体' if direction == 't2s' else '繁体'}中文中...", log_callback)
    process_directory(convert_dir, cc, log_callback)

    shutil.make_archive(os.path.splitext(output_zip)[0], 'zip', convert_dir)
    log(f"✅ ZIP 转换完成：{output_zip}", log_callback)
    return output_zip

# 转换单个目录
def convert_directory(dir_path, direction, log_callback):
    cc = OpenCC(direction)
    log(f"→ 正在转换目录：{dir_path}", log_callback)
    process_directory(dir_path, cc, log_callback)
    log("✅ 目录转换完成", log_callback)
    return dir_path

# 转换单个文件
def convert_single_file(file_path, direction, log_callback):
    cc = OpenCC(direction)
    log(f"→ 正在转换文件：{file_path}", log_callback)
    convert_file(file_path, cc, log_callback)
    log("✅ 文件转换完成", log_callback)
    return file_path

# 打开输出文件夹
def open_output_folder(file_path):
    folder = os.path.abspath(os.path.dirname(file_path))
    if sys.platform == 'win32':
        os.startfile(folder)
    elif sys.platform == 'darwin':
        subprocess.run(['open', folder])
    else:
        subprocess.run(['xdg-open', folder])

# GUI 主程序
def main():
    selected_path = None

    def log_gui(message):
        log_area.insert(tk.END, message + '\n')
        log_area.see(tk.END)

    def select_input():
        nonlocal selected_path
        file_or_dir = filedialog.askopenfilename(
            title="选择文件或 ZIP",
            filetypes=[("All supported", "*.zip *.html *.js *.json *.css *.txt *.md *.py *.vue *.ts *.php"), 
                       ("ZIP files", "*.zip"), ("All files", "*.*")]
        )
        if not file_or_dir:
            dir_path = filedialog.askdirectory(title="或选择目录进行批量转换")
            if dir_path:
                selected_path = dir_path
                log_area.delete(1.0, tk.END)
                log_gui(f"📁 选择的目录：{dir_path}")
                convert_btn.config(state="normal")
        else:
            selected_path = file_or_dir
            log_area.delete(1.0, tk.END)
            log_gui(f"📄 选择的文件：{file_or_dir}")
            convert_btn.config(state="normal")

    def start_conversion():
        if not selected_path:
            messagebox.showwarning("未选择输入", "请先选择文件或目录")
            return

        direction = direction_var.get()
        try:
            if selected_path.lower().endswith('.zip'):
                output_path = convert_zip_file(selected_path, direction, log_gui)
            elif os.path.isdir(selected_path):
                output_path = convert_directory(selected_path, direction, log_gui)
            else:
                output_path = convert_single_file(selected_path, direction, log_gui)
            messagebox.showinfo("完成", f"已完成转换：{output_path}")
            open_btn.output_path = output_path
            open_btn.config(state="normal")
        except Exception as e:
            log_gui(f"[×] 转换失败：{e}")
            messagebox.showerror("错误", str(e))

    def open_output():
        if hasattr(open_btn, 'output_path'):
            open_output_folder(open_btn.output_path)

    # 构建 GUI
    root = tk.Tk()
    root.title("繁简互转批量工具（支持目录 & ZIP）")
    root.geometry("680x500")
    root.resizable(False, False)

    # 顶部说明
    tk.Label(root, text="📌 支持 ZIP、目录或单个文件，自动批量繁简互转", font=("Arial", 11)).pack(pady=6)

    # 转换方向
    direction_var = tk.StringVar(value="t2s")
    direction_frame = tk.Frame(root)
    tk.Label(direction_frame, text="转换方向：", font=("Arial", 11)).pack(side=tk.LEFT, padx=5)
    tk.Radiobutton(direction_frame, text="繁体 ➜ 简体", variable=direction_var, value="t2s").pack(side=tk.LEFT)
    tk.Radiobutton(direction_frame, text="简体 ➜ 繁体", variable=direction_var, value="s2t").pack(side=tk.LEFT)
    direction_frame.pack(pady=5)

    # 按钮区
    btn_frame = tk.Frame(root)
    tk.Button(btn_frame, text="选择文件或目录", command=select_input, font=("Arial", 11)).pack(side=tk.LEFT, padx=10)
    convert_btn = tk.Button(btn_frame, text="开始转换", command=start_conversion, font=("Arial", 11), state="disabled")
    convert_btn.pack(side=tk.LEFT, padx=10)
    open_btn = tk.Button(btn_frame, text="打开输出目录", command=open_output, font=("Arial", 11), state="disabled")
    open_btn.pack(side=tk.LEFT, padx=10)
    btn_frame.pack(pady=10)

    # 日志显示
    log_area = scrolledtext.ScrolledText(root, wrap=tk.WORD, height=20, font=("Courier New", 10))
    log_area.pack(padx=10, pady=5, fill=tk.BOTH, expand=True)

    root.mainloop()

if __name__ == '__main__':
    main()
