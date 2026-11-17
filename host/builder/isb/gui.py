"""The window: six pages, one step each, every problem with what to do about it.

All the slow work (hashes, docker, the build, the copy) runs on threads; they
hand results back through a queue the window drains every 100 ms, since Tk
must only be touched from its own thread."""
from __future__ import annotations

import collections
import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from . import VERSION, checks, dockerctl, report, sdcard, steam, system, texts
from .checks import FAIL, INFO, OK, WARN, Item
from .progress import Progress
from .texts import t

COLOURS = {OK: "#1a7f37", WARN: "#b35900", FAIL: "#c62828", INFO: "#1f5fbf"}
MARKS = {OK: "✔", WARN: "⚠", FAIL: "✘", INFO: "ℹ"}
WRAP = 660


class Scroll(ttk.Frame):
    """A page that grows a scroll bar when it does not fit (a 768-pixel laptop)."""

    def __init__(self, parent):
        super().__init__(parent)
        bg = ttk.Style().lookup("TFrame", "background") or None
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, background=bg)
        self.bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas)
        self.window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner.bind("<Configure>", self.fit)
        self.canvas.bind("<Configure>", self.resize)
        self.canvas.bind("<Enter>", lambda e: self.canvas.bind_all("<MouseWheel>", self.wheel)
                         or self.canvas.bind_all("<Button-4>", self.wheel) or self.canvas.bind_all("<Button-5>", self.wheel))
        self.canvas.bind("<Leave>", lambda e: [self.canvas.unbind_all(x) for x in ("<MouseWheel>", "<Button-4>",
                                                                                   "<Button-5>")])

    def resize(self, event) -> None:
        self.canvas.itemconfigure(self.window, width=event.width)
        self.fit()

    def fit(self, event=None) -> None:
        self.canvas.configure(scrollregion=(0, 0, self.inner.winfo_reqwidth(), self.inner.winfo_reqheight()))
        if self.inner.winfo_reqheight() > self.canvas.winfo_height() > 1:
            self.bar.pack(side="right", fill="y")
        else:
            self.bar.pack_forget()
            self.canvas.yview_moveto(0)

    def wheel(self, event) -> None:
        if not self.bar.winfo_ismapped():
            return
        step = -1 if getattr(event, "num", 0) == 4 or getattr(event, "delta", 0) > 0 else 1
        self.canvas.yview_scroll(step * 3, "units")


class CheckList(ttk.Frame):
    """A column of ✔ / ⚠ / ✘ lines, each with a button for its fix when it has one."""

    def __init__(self, parent, app: "App"):
        super().__init__(parent)
        self.app = app

    def show(self, items: list[Item]) -> None:
        for child in self.winfo_children():
            child.destroy()
        for row, item in enumerate(items):
            ttk.Label(self, text=MARKS.get(item.state, "·"), foreground=COLOURS.get(item.state),
                      font=("TkDefaultFont", 12, "bold")).grid(row=row, column=0, sticky="nw", padx=(0, 6), pady=2)
            text = system.redact(t(item.key, **item.args))
            if item.key == "docker.memory_low" and sys.platform == "win32":
                text += "\n" + t("docker.memory_low_win")
            ttk.Label(self, text=text, wraplength=WRAP - 110, justify="left").grid(row=row, column=1, sticky="w", pady=2)
            buttons = [b for b in (self.app.fix_button(f) for f in item.fixes) if b]
            if buttons:
                box = ttk.Frame(self)
                for text, command in buttons:
                    ttk.Button(box, text=text, command=command).pack(fill="x", pady=(0, 2))
                box.grid(row=row, column=2, sticky="ne", padx=(8, 0), pady=2)
        self.columnconfigure(1, weight=1)


class App:
    def __init__(self, root: tk.Tk, args):
        self.root = root
        self.args = args
        self.calls: queue.Queue = queue.Queue()
        self.stderr_tail: collections.deque = collections.deque(maxlen=40)

        self.game: steam.GameInstall | None = None
        self.game_items: list[Item] = []
        self.exe: Path | None = getattr(args, "exe", None)
        self.exe_items: list[Item] = []
        self.docker: dockerctl.Status | None = None
        self.docker_items: list[Item] = []
        self.docker_poll = None     # the after() id of the next quiet Docker check
        self.games: list[steam.GameInstall] = []
        self.image_ok = False
        self.all_items: list[Item] = []
        self.progress: Progress | None = None
        self.running = False
        self.stopping = False
        self.busy = False          # an image download or build, or a copy

        root.title("%s %s" % (t("app.title"), VERSION))
        root.minsize(760, 600)
        self.style()
        self.folder = tk.StringVar(value=str(getattr(args, "folder", None) or system.default_folder()))
        self.game_var = tk.StringVar(value=str(args.game) if getattr(args, "game", None) else "")
        self.exe_var = tk.StringVar(value=str(self.exe) if self.exe else "")
        self.steam_var = tk.BooleanVar(value=False)
        self.threads_var = tk.StringVar()
        self.jobs_var = tk.StringVar()

        self.header = ttk.Label(root, style="Title.TLabel")
        self.header.pack(fill="x", padx=16, pady=(14, 4))
        self.body = ttk.Frame(root)
        self.body.pack(fill="both", expand=True, padx=16)
        foot = ttk.Frame(root)
        foot.pack(fill="x", padx=16, pady=12)
        self.back_btn = ttk.Button(foot, text=t("nav.back"), command=self.back)
        self.back_btn.pack(side="left")
        ttk.Button(foot, text=t("nav.report"), command=self.copy_report).pack(side="left", padx=8)
        self.next_btn = ttk.Button(foot, text=t("nav.next"), command=self.next, style="Accent.TButton")
        self.next_btn.pack(side="right")

        self.pages = [self.page_game(), self.page_exe(), self.page_docker(), self.page_ready(), self.page_build(),
                      self.page_result()]
        self.titles = ["page.game", "page.exe", "page.docker", "page.options", "page.build", "page.result"]
        self.index = 0
        self.show(0)
        root.protocol("WM_DELETE_WINDOW", self.quit)
        root.after(100, self.pump)
        self.check_game()

    # ---- plumbing ------------------------------------------------------------------------

    def style(self) -> None:
        s = ttk.Style()
        if sys.platform != "win32" and "clam" in s.theme_names():
            s.theme_use("clam")
        s.configure("Title.TLabel", font=("TkDefaultFont", 16, "bold"))
        s.configure("Notice.TFrame", background="#fff4ce")
        s.configure("Notice.TLabel", background="#fff4ce", foreground="#5c4400")
        s.configure("NoticeTitle.TLabel", background="#fff4ce", foreground="#5c4400",
                    font=("TkDefaultFont", 11, "bold"))
        s.configure("Small.TLabel", foreground="#666666")
        s.configure("Danger.TLabel", foreground=COLOURS[FAIL])
        s.configure("Accent.TButton", font=("TkDefaultFont", 10, "bold"))

    def later(self, fn, *a) -> None:
        """Runs fn(*a) on the Tk thread."""
        self.calls.put((fn, a))

    def pump(self) -> None:
        try:
            while True:
                fn, a = self.calls.get_nowait()
                fn(*a)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def background(self, work, done=None) -> None:
        def run():
            try:
                result = work()
            except Exception as e:          # shown, never swallowed
                result = e
            if done:
                self.later(done, result)
        threading.Thread(target=run, daemon=True).start()

    def show(self, index: int) -> None:
        self.pages[self.index].pack_forget()
        self.index = index
        self.pages[index].pack(fill="both", expand=True)
        self.header.configure(text=t(self.titles[index]))
        self.refresh_nav()

    def refresh_nav(self) -> None:
        i = self.index
        can_next = {0: self.game_items and not checks.blocking(self.game_items),
                    1: self.exe_items and not checks.blocking(self.exe_items),
                    2: self.docker_items and not checks.blocking(self.docker_items) and self.image_ok,
                    3: False,
                    4: bool(self.progress and self.progress.result),
                    5: False}[i]
        self.next_btn.configure(state="normal" if can_next and not self.busy else "disabled")
        self.next_btn.pack_forget() if i in (3, 5) else self.next_btn.pack(side="right")
        self.back_btn.configure(state="disabled" if i == 0 or self.running or self.busy else "normal")

    def next(self) -> None:
        if self.index < len(self.pages) - 1:
            self.show(self.index + 1)
            if self.index == 1 and not self.exe_items:
                self.check_exe() if self.exe_var.get() else self.search_exe()
            elif self.index == 2 and not self.docker_items:
                self.check_docker()

    def back(self) -> None:
        if self.index > 0:
            self.show(self.index - 1)

    def fix_button(self, fix: str | None):
        actions = {
            "choose_game": (t("btn.choose"), self.choose_game),
            "verify_steam": (t("btn.verify_steam"), lambda: system.open_uri("steam://validate/%s" % steam.APPID)),
            "choose_exe": (t("btn.choose"), self.choose_exe),
            "install_docker": (t("btn.install_docker"), self.install_docker),
            "docker_page": (t("btn.docker_page"), lambda: system.open_url(system.docker_url())),
            "start_docker": (t("btn.start_docker"), self.start_docker),
            "copy_group": (t("btn.copy_cmd"), lambda: self.copy_text(system.COMMANDS["copy_group"])),
            "copy_start": (t("btn.copy_cmd"), lambda: self.copy_text(system.COMMANDS["copy_start"])),
            "retry": (t("btn.check_again"), self.check_docker),
            "wslconfig": (t("btn.wslconfig"), system.edit_wslconfig),
        }
        return actions.get(fix) if fix else None

    def copy_text(self, text: str) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        messagebox.showinfo(t("app.title"), t("copied") + "\n\n" + text, parent=self.root)

    def notice(self, parent, short: bool = False) -> ttk.Frame:
        box = ttk.Frame(parent, style="Notice.TFrame", padding=12)
        if short:
            ttk.Label(box, text="⚠  " + t("drm.short"), style="NoticeTitle.TLabel", wraplength=WRAP).pack(anchor="w")
        else:
            ttk.Label(box, text="⚠  " + t("drm.title"), style="NoticeTitle.TLabel").pack(anchor="w")
            ttk.Label(box, text=t("drm.body", sha=checks.PE_SHA256), style="Notice.TLabel", wraplength=WRAP,
                      justify="left").pack(anchor="w", pady=(6, 0))
        return box

    def path_row(self, parent, label: str, var: tk.StringVar, choose, picked=None) -> ttk.Frame:
        """A path to type or paste, a list of suggestions when picked is given, and Choose…."""
        row = ttk.Frame(parent)
        if label:
            ttk.Label(row, text=label).pack(side="left")
        if picked:
            entry = ttk.Combobox(row, textvariable=var)
            entry.bind("<<ComboboxSelected>>", lambda e: picked())
            entry.bind("<Return>", lambda e: picked())
            row.combo = entry
        else:
            entry = ttk.Entry(row, textvariable=var)
        entry.pack(side="left", fill="x", expand=True, padx=8)
        # A long path shows its end, the part that tells files apart.
        var.trace_add("write", lambda *_: entry.after_idle(entry.xview_moveto, 1.0))
        ttk.Button(row, text=t("btn.choose"), command=choose).pack(side="left")
        return row

    # ---- 1. the game -----------------------------------------------------------------------

    def page_game(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        self.notice(f).pack(fill="x", pady=(0, 12))
        ttk.Label(f, text=t("page.game.intro"), wraplength=WRAP, justify="left").pack(anchor="w")
        row = self.path_row(f, t("page.game.folder"), self.game_var, self.choose_game,
                            picked=lambda: self.check_game(Path(self.game_var.get())))
        row.pack(fill="x", pady=10)
        self.game_combo = row.combo
        self.game_list = CheckList(f, self)
        self.game_list.pack(fill="x")
        return page

    def choose_game(self) -> None:
        start = self.game_var.get() or (str(steam.steam_roots()[0] / "steamapps" / "common")
                                        if steam.steam_roots() else str(Path.home()))
        path = filedialog.askdirectory(parent=self.root, initialdir=start, mustexist=True)
        if path:
            self.game_var.set(path)
            self.check_game(Path(path))

    def check_game(self, chosen: Path | None = None) -> None:
        if chosen is None and self.game_var.get():
            chosen = Path(self.game_var.get())

        def work():
            games = steam.find_games()
            if chosen:
                known = [g for g in games if g.path.resolve() == chosen.resolve()] if chosen.exists() else []
                game = known[0] if known else steam.describe_folder(chosen)
            else:
                game = games[0] if games else None
            return games, game, checks.check_game(game)

        def done(result):
            if isinstance(result, Exception):
                result = ([], None, [Item(FAIL, "game.not_found", fix="choose_game")])
            self.games, self.game, self.game_items = result
            self.game_combo.configure(values=[str(g.path) for g in self.games])
            if self.game and not self.game_var.get():
                self.game_var.set(str(self.game.path))
            items = list(self.game_items)
            if len(self.games) > 1:
                items.append(Item(INFO, "game.others", {"n": len(self.games)}))
            self.game_list.show(items)
            self.refresh_nav()

        self.game_list.show([Item(WARN, "cli.checking")])
        self.background(work, done)

    # ---- 2. the executable -------------------------------------------------------------------

    def page_exe(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        self.notice(f, short=True).pack(fill="x", pady=(0, 12))
        ttk.Label(f, text=t("page.exe.intro"), wraplength=WRAP, justify="left").pack(anchor="w")
        row = self.path_row(f, "", self.exe_var, self.choose_exe, picked=self.check_exe)
        ttk.Button(row, text=t("btn.search"), command=self.search_exe).pack(side="left", padx=(8, 0))
        row.pack(fill="x", pady=10)
        self.exe_combo = row.combo
        self.exe_bar = ttk.Progressbar(f, maximum=1.0)
        self.exe_list = CheckList(f, self)
        self.exe_list.pack(fill="x")
        return page

    def choose_exe(self) -> None:
        path = filedialog.askopenfilename(parent=self.root, filetypes=[("Windows executable", "*.exe"), ("*", "*")],
                                          initialdir=str(Path(self.exe_var.get()).parent) if self.exe_var.get()
                                          else str(system.suggested_start_dir()))
        if path:
            self.exe_var.set(path)
            self.check_exe()

    def search_exe(self) -> None:
        self.exe_list.show([Item(WARN, "exe.searching")])

        game = self.game.path if self.game else None

        def work():
            dirs = checks.exe_search_dirs(game)
            return checks.find_exe(dirs), checks.exe_candidates(dirs, game=game)

        def done(result):
            found, candidates = result if not isinstance(result, Exception) else (None, [])
            self.exe_combo.configure(values=[str(c) for c in candidates])
            if isinstance(found, Path):
                self.exe_var.set(str(found))
                self.check_exe()
            else:
                self.exe_items = []
                items = [Item(WARN, "exe.not_found", fix="choose_exe")]
                if candidates:
                    items.append(Item(INFO, "exe.suggested", {"n": len(candidates)}))
                self.exe_list.show(items)
                self.refresh_nav()

        self.background(work, done)

    def check_exe(self) -> None:
        path = Path(self.exe_var.get()) if self.exe_var.get() else None
        self.exe_bar.pack(fill="x", pady=(0, 8), before=self.exe_list)
        self.exe_bar["value"] = 0
        self.exe_list.show([Item(WARN, "exe.hashing")])

        def work():
            return checks.check_exe(path, lambda frac: self.later(self.exe_bar.configure, {"value": frac}))

        def done(items):
            self.exe_bar.pack_forget()
            if isinstance(items, Exception):
                items = [Item(FAIL, "exe.missing", fix="choose_exe")]
            self.exe = path
            self.exe_items = items
            self.exe_list.show(items)
            self.refresh_nav()

        self.background(work, done)

    # ---- 3. Docker ----------------------------------------------------------------------------

    def page_docker(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        ttk.Label(f, text=t("page.docker.intro"), wraplength=WRAP, justify="left").pack(anchor="w", pady=(0, 10))
        self.docker_list = CheckList(f, self)
        self.docker_list.pack(fill="x")
        row = ttk.Frame(f)
        row.pack(fill="x", pady=10)
        ttk.Button(row, text=t("btn.check_again"), command=self.check_docker).pack(side="left")
        self.image_btn = ttk.Button(row, command=self.get_image)
        self.image_bar = ttk.Progressbar(f, mode="indeterminate")
        self.image_line = ttk.Label(f, style="Small.TLabel", wraplength=WRAP)
        self.docker_wait = ttk.Label(f, text=t("docker.waiting"), style="Small.TLabel", wraplength=WRAP)
        return page

    def schedule_docker_poll(self) -> None:
        if self.docker_poll:
            self.root.after_cancel(self.docker_poll)
        self.docker_poll = self.root.after(5000, self.poll_docker)

    def poll_docker(self) -> None:
        """A quiet check while the Docker page waits for Docker to be installed or started."""
        self.docker_poll = None
        if self.index != 2 or self.busy or (self.docker and self.docker.state == "ok"):
            return
        before = self.docker.state if self.docker else None

        def done(st):
            if isinstance(st, Exception) or st.state == before:
                self.schedule_docker_poll()
            else:
                self.check_docker()

        self.background(dockerctl.status, done)

    def install_docker(self) -> None:
        if self.busy:
            return
        self.busy = True
        self.image_bar.pack(fill="x", pady=(4, 2))
        self.image_line.pack(anchor="w")
        self.image_bar.start(12)
        self.image_line.configure(text=t("docker.installing"))
        self.refresh_nav()

        def line(text):
            self.stderr_tail.append(text)
            self.later(self.image_line.configure, {"text": text[:160]})

        def done(rc):
            self.busy = False
            self.image_bar.stop()
            self.image_bar.pack_forget()
            rc = 1 if isinstance(rc, Exception) else rc
            self.image_line.configure(text=t("docker.installed") if rc == 0 else t("docker.install_failed", rc=rc))
            self.check_docker()

        self.background(lambda: system.install_docker(line), done)

    def start_docker(self) -> None:
        system.start_docker_desktop()
        self.schedule_docker_poll()

    def check_docker(self) -> None:
        self.docker_list.show([Item(WARN, "cli.checking")])
        self.image_btn.pack_forget()
        self.docker_wait.pack_forget()

        def work():
            st = dockerctl.status()
            items = dockerctl.status_items(st)
            image_ok, image_item, action = False, None, None
            if st.state == "ok":
                ref = dockerctl.image_ref(getattr(self.args, "image", None))
                if dockerctl.image_present(st.exe, ref):
                    image_ok, image_item = True, Item(OK, "image.present", {"ref": ref})
                elif dockerctl.image_is_published(ref):
                    image_item, action = Item(WARN, "image.absent"), "pull"
                elif dockerctl.source_tree():
                    image_item, action = Item(WARN, "image.unpublished"), "build"
                else:
                    image_item = Item(FAIL, "image.unavailable")
            return st, items + ([image_item] if image_item else []), image_ok, action

        def done(result):
            if isinstance(result, Exception):
                result = (dockerctl.Status("error", detail=str(result)),
                          [Item(FAIL, "docker.stopped", {"detail": str(result)}, fix="retry")], False, None)
            self.docker, self.docker_items, self.image_ok, action = result
            self.docker_list.show(self.docker_items)
            if self.docker.state != "ok":
                self.docker_wait.pack(anchor="w", pady=(6, 0))
                self.schedule_docker_poll()
            if action:
                self.image_btn.configure(text=t("btn.get_image" if action == "pull" else "btn.build_image"))
                self.image_btn.pack(side="left", padx=8)
                self.image_action = action
            self.refresh_nav()

        self.background(work, done)

    def get_image(self) -> None:
        st = self.docker
        if not st or st.state != "ok":
            return
        ref = dockerctl.image_ref(getattr(self.args, "image", None))
        self.busy = True
        self.image_btn.configure(state="disabled")
        self.image_bar.pack(fill="x", pady=(4, 2))
        self.image_line.pack(anchor="w")
        self.image_bar.start(12)
        self.image_line.configure(text=t("image.pulling" if self.image_action == "pull" else "image.building"))
        self.refresh_nav()

        def line(text):
            self.stderr_tail.append(text)
            self.later(self.image_line.configure, {"text": text[:160]})

        def work():
            if self.image_action == "pull":
                return dockerctl.pull(st.exe, ref, line)
            return dockerctl.build_image(st.exe, dockerctl.source_tree(), ref, line)

        def done(rc):
            self.busy = False
            self.image_bar.stop()
            self.image_bar.pack_forget()
            self.image_btn.configure(state="normal")
            if rc != 0:
                self.image_line.configure(text=t("image.failed"))
            else:
                self.image_line.pack_forget()
            self.check_docker()

        self.background(work, done)

    # ---- 4. ready ---------------------------------------------------------------------------------

    def page_ready(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        ttk.Label(f, text=t("page.options.intro"), wraplength=WRAP, justify="left").pack(anchor="w")
        self.path_row(f, t("page.options.folder"), self.folder, self.choose_folder).pack(fill="x", pady=(10, 2))
        if system.uses_volume():
            ttk.Label(f, text=t("page.options.volume"), style="Small.TLabel", wraplength=WRAP).pack(anchor="w")

        self.adv_open = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text=t("page.options.advanced"), variable=self.adv_open,
                        command=self.toggle_advanced).pack(anchor="w", pady=(10, 0))
        self.adv = ttk.Frame(f, padding=(20, 4, 0, 4))
        ttk.Checkbutton(self.adv, text=t("page.options.steam"), variable=self.steam_var).pack(anchor="w")
        ttk.Label(self.adv, text=t("page.options.steam_warning"), style="Small.TLabel", wraplength=WRAP - 40,
                  justify="left").pack(anchor="w", padx=(22, 0))
        for label, var in ((t("page.options.threads"), self.threads_var), (t("page.options.jobs"), self.jobs_var)):
            row = ttk.Frame(self.adv)
            ttk.Label(row, text=label).pack(side="left")
            ttk.Entry(row, textvariable=var, width=6).pack(side="left", padx=6)
            row.pack(anchor="w", pady=2)
        ttk.Button(self.adv, text=t("btn.clean"), command=self.clean).pack(anchor="w", pady=(6, 0))
        self.adv_anchor = ttk.Frame(f)
        self.adv_anchor.pack(fill="x")

        row = ttk.Frame(f)
        row.pack(fill="x", pady=12)
        ttk.Button(row, text=t("btn.check_all"), command=self.check_all).pack(side="left")
        self.start_btn = ttk.Button(row, text=t("btn.start"), command=self.start, style="Accent.TButton")
        self.start_btn.pack(side="right")
        self.all_summary = ttk.Label(f, font=("TkDefaultFont", 10, "bold"))
        self.all_summary.pack(anchor="w")
        self.all_list = CheckList(f, self)
        self.all_list.pack(fill="x")
        return page

    def toggle_advanced(self) -> None:
        if self.adv_open.get():
            self.adv.pack(fill="x", before=self.adv_anchor)
        else:
            self.adv.pack_forget()

    def choose_folder(self) -> None:
        path = filedialog.askdirectory(parent=self.root, initialdir=str(Path.home()))
        if path:
            self.folder.set(str(Path(path) / "IsaacSwitchBuild") if Path(path) == Path.home() else path)

    def check_all(self, then=None) -> None:
        self.all_summary.configure(text=t("cli.checking"), foreground="")
        self.start_btn.configure(state="disabled")
        exe = Path(self.exe_var.get()) if self.exe_var.get() else None
        game_path = Path(self.game_var.get()) if self.game_var.get() else None
        folder = Path(self.folder.get())
        ref = dockerctl.image_ref(getattr(self.args, "image", None))

        def work():
            game = steam.describe_folder(game_path) if game_path else steam.find_game()
            items = checks.check_game(game) + checks.check_exe(exe)
            st = dockerctl.status()
            items += dockerctl.status_items(st)
            if st.state == "ok":
                present = dockerctl.image_present(st.exe, ref)
                items.append(Item(OK, "image.present", {"ref": ref}) if present else Item(FAIL, "image.absent"))
            items += checks.check_disk(folder, checks.NEED_DISK if not system.uses_volume() else 2 * checks.GIB)
            return game, st, items

        def done(result):
            self.start_btn.configure(state="normal")
            if isinstance(result, Exception):
                self.all_summary.configure(text=str(result), foreground=COLOURS[FAIL])
                return
            self.game, self.docker, self.all_items = result
            self.all_list.show(self.all_items)
            bad = checks.blocking(self.all_items)
            self.all_summary.configure(text=t("check.some_fail" if bad else "check.all_ok"),
                                       foreground=COLOURS[FAIL if bad else OK])
            if then and not bad:
                then()

        self.background(work, done)

    def plan(self) -> dockerctl.Plan:
        base = Path(self.folder.get())

        def number(var):
            try:
                n = int(var.get())
                return n if n > 0 else None
            except ValueError:
                return None

        return dockerctl.Plan(engine=self.docker.engine, exe_path=self.docker.exe,
                              image=dockerctl.image_ref(getattr(self.args, "image", None)),
                              game_exe=Path(self.exe_var.get()).resolve(), game=self.game.path.resolve(),
                              work=system.work_folder(base), export=system.export_folder(base),
                              steam=self.steam_var.get(), threads=number(self.threads_var),
                              jobs=number(self.jobs_var))

    def clean(self) -> None:
        if self.running:
            return
        work = system.work_folder(Path(self.folder.get()))
        what = dockerctl.VOLUME if work is None else str(work)
        if not messagebox.askyesno(t("btn.clean"), t("clean.confirm", what=what),
                                   parent=self.root):
            return
        found = dockerctl.find_engine()

        def done(ok):
            messagebox.showinfo(t("btn.clean"), t("cli.cleaned") if ok is True else t("cli.clean_refused", path=what),
                                parent=self.root)

        self.background(lambda: dockerctl.remove_work(found[1] if found else "", work), done)

    # ---- 5. the build ----------------------------------------------------------------------------

    def page_build(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        self.stage_rows: dict[str, tuple] = {}
        grid = ttk.Frame(f)
        grid.pack(fill="x", pady=(0, 10))
        names = Progress(export=system.uses_volume()).names
        for row, name in enumerate(names):
            mark = ttk.Label(grid, text="·", width=2, font=("TkDefaultFont", 11, "bold"))
            mark.grid(row=row, column=0, sticky="w")
            ttk.Label(grid, text=t("stage." + name)).grid(row=row, column=1, sticky="w", pady=1)
            took = ttk.Label(grid, style="Small.TLabel")
            took.grid(row=row, column=2, sticky="e", padx=(16, 0))
            self.stage_rows[name] = (mark, took)
        self.bar = ttk.Progressbar(f, maximum=1.0)
        self.bar.pack(fill="x")
        self.time_label = ttk.Label(f)
        self.time_label.pack(anchor="w", pady=(4, 0))
        self.log_label = ttk.Label(f, style="Small.TLabel", wraplength=WRAP)
        self.log_label.pack(anchor="w", pady=(2, 8))
        self.message = ttk.Label(f, wraplength=WRAP, justify="left")
        self.message.pack(anchor="w")
        self.detail = tk.Text(f, height=8, wrap="none", font=("TkFixedFont", 9), relief="flat",
                              background="#f3f3f3")
        row = ttk.Frame(f)
        row.pack(fill="x", side="bottom", pady=6)
        self.stop_btn = ttk.Button(row, text=t("btn.cancel"), command=self.stop)
        self.stop_btn.pack(side="left")
        self.resume_btn = ttk.Button(row, text=t("btn.resume"), command=self.start)
        return page

    def start(self) -> None:
        if self.running:
            return
        if self.index == 3:
            self.check_all(then=self.launch)
        else:
            self.launch()

    def launch(self) -> None:
        plan = self.plan()
        self.progress = Progress(export=plan.export is not None)
        self.running, self.stopping = True, False
        for name, (mark, took) in self.stage_rows.items():
            mark.configure(text="·", foreground="")
            took.configure(text="")
        self.message.configure(text=t("page.build.starting"), foreground="")
        self.detail.pack_forget()
        self.resume_btn.pack_forget()
        self.stop_btn.configure(state="normal")
        self.stop_btn.pack(side="left")
        self.show(4)
        self.tick()

        def on_event(event):
            self.later(self.on_event, event)

        def on_text(line):
            self.stderr_tail.append(line)

        def work():
            with system.keep_awake("Building Isaac for the Switch"):
                return dockerctl.run_build(plan, on_event, on_text)

        self.background(work, self.finished)

    def on_event(self, event: dict) -> None:
        p = self.progress
        p.feed(event)
        kind = event.get("event")
        self.render_stages()
        if kind == "stage" or kind in ("exporting", "exported"):
            self.message.configure(text="")
        elif kind == "log":
            self.log_label.configure(text=event.get("line", "")[:200])
        elif kind == "problems":
            self.message.configure(text=t("page.build.problems"), foreground=COLOURS[FAIL])
            self.show_detail("\n".join("- " + x for x in p.problems))
        elif kind == "failed":
            self.message.configure(text=t("page.build.failed", message=event.get("message", "")),
                                   foreground=COLOURS[FAIL])
            self.show_detail("\n".join(event.get("tail", [])))
        elif kind == "done":
            self.bar["value"] = 1.0

    def render_stages(self) -> None:
        p = self.progress
        for name, state in p.state.items():
            mark, took = self.stage_rows.get(name, (None, None))
            if not mark:
                continue
            sym, colour = {"done": ("✔", COLOURS[OK]), "skipped": ("✔", COLOURS[OK]),
                           "running": ("▶", "#1565c0"), "failed": ("✘", COLOURS[FAIL])}.get(state, ("·", ""))
            mark.configure(text=sym, foreground=colour)
            if state == "done" and name in p.took:
                took.configure(text=texts.duration(p.took[name]))
            elif state == "skipped":
                took.configure(text="✓")
            else:
                took.configure(text="")

    def show_detail(self, text: str) -> None:
        self.detail.configure(state="normal")
        self.detail.delete("1.0", "end")
        self.detail.insert("1.0", system.redact(text))
        self.detail.configure(state="disabled")
        self.detail.pack(fill="both", expand=True, before=self.stop_btn.master)

    def tick(self) -> None:
        if not self.running or not self.progress:
            return
        p = self.progress
        self.bar["value"] = p.fraction()
        left = p.eta()
        elapsed = texts.duration(p.elapsed())
        if left and p.current:
            self.time_label.configure(text=t("page.build.eta", elapsed=elapsed, left=texts.duration(left)))
        else:
            self.time_label.configure(text=t("page.build.elapsed", elapsed=elapsed))
        self.root.after(1000, self.tick)

    def stop(self) -> None:
        if not self.running or not self.docker:
            return
        self.stopping = True
        self.stop_btn.configure(state="disabled")
        self.background(lambda: dockerctl.stop(self.docker.exe))

    def finished(self, rc) -> None:
        self.running = False
        p = self.progress
        self.stop_btn.pack_forget()
        self.log_label.configure(text="")
        if p:
            self.time_label.configure(text=t("page.build.elapsed", elapsed=texts.duration(p.elapsed())))
            self.render_stages()
        if p and p.result:
            self.result_path.configure(text=str(system.result_folder(Path(self.folder.get()))))
            self.result_hash.configure(text=t("page.result.hash", sha=p.result.get("nro_sha256", "")))
            self.refresh_nav()
            self.show(5)
            self.refresh_drives()
            return
        if isinstance(rc, Exception):
            self.message.configure(text=t("page.build.failed", message=rc), foreground=COLOURS[FAIL])
        elif self.stopping or (p and p.stopped):
            self.message.configure(text=t("page.build.stopped"), foreground="")
        elif not (p and (p.failure or p.problems)):
            self.message.configure(text=t("page.build.failed", message="exit %s" % rc), foreground=COLOURS[FAIL])
            self.show_detail("\n".join(self.stderr_tail))
        self.resume_btn.pack(side="left")
        self.refresh_nav()

    # ---- 6. the result -------------------------------------------------------------------------------

    def page_result(self) -> ttk.Frame:
        page = Scroll(self.body)
        f = page.inner
        ttk.Label(f, text=t("page.result.intro")).pack(anchor="w")
        row = ttk.Frame(f)
        row.pack(fill="x", pady=4)
        self.result_path = ttk.Label(row, font=("TkFixedFont", 10), wraplength=WRAP - 160, justify="left")
        self.result_path.pack(side="left")
        ttk.Button(row, text=t("btn.open_folder"),
                   command=lambda: system.open_path(system.result_folder(Path(self.folder.get())))).pack(side="right")
        self.result_hash = ttk.Label(f, style="Small.TLabel")
        self.result_hash.pack(anchor="w")
        ttk.Label(f, text=t("page.result.how"), wraplength=WRAP, justify="left").pack(anchor="w", pady=10)
        ttk.Label(f, text="⚠  " + t("page.result.share"), style="Danger.TLabel", wraplength=WRAP,
                  justify="left").pack(anchor="w")

        sd = ttk.Frame(f)
        sd.pack(fill="x", pady=(14, 4))
        ttk.Label(sd, text=t("page.result.sd")).pack(side="left")
        self.drive_var = tk.StringVar()
        self.drive_box = ttk.Combobox(sd, textvariable=self.drive_var, width=40)
        self.drive_box.pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(sd, text=t("btn.refresh"), command=self.refresh_drives).pack(side="left")
        ttk.Button(sd, text=t("btn.choose"), command=self.choose_drive).pack(side="left", padx=4)
        self.copy_btn = ttk.Button(sd, text=t("btn.copy_sd"), command=self.copy_sd, style="Accent.TButton")
        self.copy_btn.pack(side="left")
        self.copy_bar = ttk.Progressbar(f, maximum=1.0)
        self.copy_label = ttk.Label(f, wraplength=WRAP)
        self.copy_label.pack(anchor="w")
        return page

    def refresh_drives(self) -> None:
        self.drives = sdcard.removable_drives()
        self.drive_box["values"] = [str(d) for d in self.drives]
        if self.drives:
            self.drive_box.current(0)
            self.copy_label.configure(text="", foreground="")
        else:
            self.drive_var.set("")
            self.copy_label.configure(text=t("page.result.no_sd"), foreground="")

    def choose_drive(self) -> None:
        path = filedialog.askdirectory(parent=self.root, mustexist=True)
        if path:
            self.drive_var.set(path)

    def selected_drive(self) -> Path | None:
        text = self.drive_var.get().strip()
        for d in getattr(self, "drives", []):
            if str(d) == text:
                return d.path
        return Path(text) if text else None

    def copy_sd(self) -> None:
        dest = self.selected_drive()
        if not dest or not dest.is_dir():
            self.copy_label.configure(text=t("page.result.no_sd"), foreground=COLOURS[FAIL])
            return
        if not sdcard.looks_like_switch_card(dest) and not messagebox.askyesno(
                t("btn.copy_sd"), t("page.result.not_switch", path=dest), parent=self.root):
            return
        src = system.result_folder(Path(self.folder.get()))
        self.busy = True
        self.copy_btn.configure(state="disabled")
        self.copy_bar.pack(fill="x", pady=4, before=self.copy_label)
        self.refresh_nav()

        def progress(frac, name):
            self.later(self.copy_progress, frac, name)

        def done(result):
            self.busy = False
            self.copy_btn.configure(state="normal")
            self.copy_bar.pack_forget()
            if isinstance(result, Exception):
                self.copy_label.configure(text=t("page.result.copy_failed", error=result), foreground=COLOURS[FAIL])
            else:
                self.copy_label.configure(text=t("page.result.copied", copied=result[0], checked=result[1]),
                                          foreground=COLOURS[OK])
            self.refresh_nav()

        self.background(lambda: sdcard.copy_to_card(src, dest, progress), done)

    def copy_progress(self, frac: float, name: str) -> None:
        self.copy_bar["value"] = frac
        self.copy_label.configure(text=t("page.result.copying", name=name), foreground="")

    # ---- report and quit ------------------------------------------------------------------------------

    def copy_report(self) -> None:
        sections = {
            "Game": [report.item_line(i) for i in self.game_items],
            "Executable": [report.item_line(i) for i in self.exe_items],
            "Docker": [report.item_line(i) for i in self.docker_items]
                      + ([self.docker.detail] if self.docker and self.docker.detail else []),
            "Check": [report.item_line(i) for i in self.all_items],
        }
        p = self.progress
        if p:
            sections["Build"] = ["%s: %s%s" % (n, s, " (%.0f s)" % p.took[n] if n in p.took else "")
                                 for n, s in p.state.items()]
            if p.problems:
                sections["Problems"] = p.problems
            if p.failure:
                sections["Failure"] = [p.failure.get("message", "")] + list(p.failure.get("tail", []))
            if p.result:
                sections["Result"] = ["NRO sha256 %s" % p.result.get("nro_sha256")]
        sections["Last output"] = list(self.stderr_tail)[-20:]
        self.root.clipboard_clear()
        self.root.clipboard_append(report.build(sections))
        messagebox.showinfo(t("nav.report"), t("nav.report_done"), parent=self.root)

    def quit(self) -> None:
        if self.running:
            if not messagebox.askyesno(t("app.title"), t("nav.quit_running"), parent=self.root):
                return
            if self.docker:
                dockerctl.stop(self.docker.exe)
        self.root.destroy()


def main(args) -> int:
    try:
        root = tk.Tk()
    except tk.TclError as e:
        print("Cannot open a window (%s). Use the command line: isaac-switch-builder --help" % e, file=sys.stderr)
        return 2
    App(root, args)
    root.mainloop()
    return 0
