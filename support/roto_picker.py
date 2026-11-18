#!/usr/bin/env python3
"""Picker window for the CorridorKey Hint and Roto nodes (PySide6, runs in the engine venv).

Opened by the node's "Pick…" button on the current frame:
    roto_picker.py <job dir> <frame index> <roto_daemon.py> <sam model> [<node name> <helper python>]

Tools (P / B / L):
  Point   click: keep this · right-click (or Alt+click): remove this · drag: a box
  Box     drag a rectangle around something = a new object
  Lasso   draw around an area: adds it · Alt+draw or right-drag: cuts it away
          "Exact edge" on: the edge is where you drew; off: SAM finds the edge inside
  wheel / middle   zoom / pan, double-middle-click fits
  Ctrl+Z           undo

Every change asks the SAM 2 helper for a preview mask (shown in red). "Use this
selection" writes <job>/prompts.json, which the node's Track clip uses.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import (QBrush, QColor, QImage, QKeySequence, QPainter, QPainterPath, QPen,
                           QPixmap, QPolygonF, QShortcut)
from PySide6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QGraphicsEllipseItem,
                               QGraphicsPathItem, QGraphicsPixmapItem, QGraphicsPolygonItem,
                               QGraphicsRectItem, QGraphicsScene, QGraphicsSimpleTextItem,
                               QGraphicsView, QHBoxLayout, QLabel, QListWidget, QListWidgetItem,
                               QMainWindow, QMessageBox, QPushButton, QToolButton, QVBoxLayout,
                               QWidget)

COLORS = ["#3ddc84", "#4fc3f7", "#ffd54f", "#ff8a65", "#ba68c8", "#4db6ac", "#f06292", "#aed581"]


def replace_retry(src, dst, tries: int = 20):
    """os.replace, retried: on Windows it fails while another program (Resolve
    reading a mask) has the target open for a moment."""
    for i in range(tries):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(0.05)


def atomic_write(path: Path, text: str):
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(text)
    replace_retry(tmp, path, 50)


def read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


class View(QGraphicsView):
    def __init__(self, win):
        super().__init__()
        self.win = win
        self.setScene(QGraphicsScene(self))
        self.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform)
        self.setBackgroundBrush(QColor("#151515"))
        self.setTransformationAnchor(QGraphicsView.ViewportAnchor.AnchorUnderMouse)
        self.frame_item = QGraphicsPixmapItem()
        self.mask_item = QGraphicsPixmapItem()
        self.mask_item.setOpacity(0.5)
        self.scene().addItem(self.frame_item)
        self.scene().addItem(self.mask_item)
        self.marks = []
        self.press = None   # scene pos of the left press
        self.band = None
        self.fit_mode = True
        self.tool = "point"  # point | box | lasso
        self.lasso = None    # points being drawn
        self.lasso_op = 1
        self.lasso_item = None

    def set_frame(self, pix: QPixmap):
        self.frame_item.setPixmap(pix)
        self.scene().setSceneRect(QRectF(pix.rect()))
        self.fit()

    def set_mask(self, img: QImage | None):
        if img is None or img.isNull():
            self.mask_item.setPixmap(QPixmap())
            return
        fw = self.frame_item.pixmap().width()
        fh = self.frame_item.pixmap().height()
        img = img.convertToFormat(QImage.Format.Format_Grayscale8).scaled(
            fw, fh, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)
        over = QImage(img.size(), QImage.Format.Format_ARGB32_Premultiplied)
        over.fill(QColor("#ff3030"))
        over.setAlphaChannel(img)
        self.mask_item.setPixmap(QPixmap.fromImage(over))

    def draw_marks(self, objects, current):
        for it in self.marks:
            self.scene().removeItem(it)
        self.marks = []
        r = max(5.0, self.frame_item.pixmap().width() / 200)
        for i, ob in enumerate(objects):
            col = QColor(COLORS[i % len(COLORS)])
            sel = i == current
            if ob.get("box"):
                x0, y0, x1, y1 = ob["box"]
                it = QGraphicsRectItem(x0, y0, x1 - x0, y1 - y0)
                it.setPen(QPen(col, max(2.0, r / (2 if sel else 3)),
                               Qt.PenStyle.SolidLine if sel else Qt.PenStyle.DashLine))
                self.scene().addItem(it)
                self.marks.append(it)
            for rg in ob.get("regions", []):
                it = QGraphicsPolygonItem(QPolygonF([QPointF(x, y) for x, y in rg["poly"]]))
                c = QColor(col) if rg.get("op", 1) else QColor("#ff3b3b")
                fill = QColor(c)
                fill.setAlpha(50)
                it.setBrush(QBrush(fill))
                pen = QPen(c, 3 if sel else 2,
                           Qt.PenStyle.SolidLine if rg.get("exact", True) else Qt.PenStyle.DashLine)
                pen.setCosmetic(True)
                it.setPen(pen)
                self.scene().addItem(it)
                self.marks.append(it)
            for x, y, lab in ob.get("points", []):
                it = QGraphicsEllipseItem(x - r, y - r, 2 * r, 2 * r)
                it.setBrush(QBrush(col if lab else QColor("#ff3b3b")))
                it.setPen(QPen(QColor("white" if sel else "#222"), r / 3))
                self.scene().addItem(it)
                self.marks.append(it)
            if ob.get("box") or ob.get("points") or ob.get("regions"):
                x, y = (ob["box"][0], ob["box"][1]) if ob.get("box") else \
                    ob["points"][0][:2] if ob.get("points") else ob["regions"][0]["poly"][0]
                t = QGraphicsSimpleTextItem(str(i + 1))
                t.setBrush(col)
                t.setScale(max(1.0, r / 4))
                t.setPos(x + r, y - 5 * r)
                self.scene().addItem(t)
                self.marks.append(t)

    def fit(self):
        self.fit_mode = True
        if not self.frame_item.pixmap().isNull():
            self.fitInView(self.frame_item, Qt.AspectRatioMode.KeepAspectRatio)

    def resizeEvent(self, e):
        super().resizeEvent(e)
        if self.fit_mode:
            self.fit()

    def mousePressEvent(self, e):
        p = self.mapToScene(e.position().toPoint())
        if e.button() == Qt.MouseButton.MiddleButton:
            self.setDragMode(QGraphicsView.DragMode.ScrollHandDrag)
            fake = e.clone() if hasattr(e, "clone") else e
            return super().mousePressEvent(fake)
        if not self.frame_item.contains(p):
            return
        alt = bool(e.modifiers() & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier))
        if self.tool == "lasso" and e.button() in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton):
            self.lasso = [p]
            self.lasso_op = 0 if (alt or e.button() == Qt.MouseButton.RightButton) else 1
            self.lasso_item = QGraphicsPathItem()
            pen = QPen(QColor("#ffd54f" if self.lasso_op else "#ff3b3b"), 2)
            pen.setCosmetic(True)
            self.lasso_item.setPen(pen)
            self.scene().addItem(self.lasso_item)
            return
        if e.button() == Qt.MouseButton.RightButton:
            if self.tool == "point":
                self.win.add_point(p.x(), p.y(), 0)
        elif e.button() == Qt.MouseButton.LeftButton:
            if alt and self.tool == "point":
                self.win.add_point(p.x(), p.y(), 0)
            else:
                self.press = p

    def mouseMoveEvent(self, e):
        if self.lasso is not None:
            p = self.mapToScene(e.position().toPoint())
            r = self.frame_item.boundingRect()
            p = QPointF(min(max(p.x(), r.left()), r.right()), min(max(p.y(), r.top()), r.bottom()))
            if (p - self.lasso[-1]).manhattanLength() * self.transform().m11() >= 3:
                self.lasso.append(p)
                path = QPainterPath(self.lasso[0])
                for q in self.lasso[1:]:
                    path.lineTo(q)
                self.lasso_item.setPath(path)
            return
        if self.press is not None:
            p = self.mapToScene(e.position().toPoint())
            d = (p - self.press)
            if self.band is None and (abs(d.x()) + abs(d.y())) * self.transform().m11() > 8:
                self.band = QGraphicsRectItem(QRectF(self.press, p))
                self.band.setPen(QPen(QColor("#ffd54f"), 2, Qt.PenStyle.DashLine))
                self.band.pen().setCosmetic(True)
                self.scene().addItem(self.band)
            if self.band is not None:
                self.band.setRect(QRectF(self.press, p).normalized())
            return
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e):
        if self.lasso is not None:
            pts, op = self.lasso, self.lasso_op
            self.scene().removeItem(self.lasso_item)
            self.lasso = self.lasso_item = None
            if len(pts) >= 3:
                poly = QPolygonF(pts)
                if poly.boundingRect().width() > 3 and poly.boundingRect().height() > 3:
                    self.win.add_region([[round(q.x(), 1), round(q.y(), 1)] for q in pts], op)
            return
        if self.press is not None and e.button() == Qt.MouseButton.LeftButton:
            if self.band is not None:
                rect = self.band.rect().intersected(self.frame_item.boundingRect())
                self.scene().removeItem(self.band)
                self.band = None
                if rect.width() > 4 and rect.height() > 4:
                    self.win.add_box(rect.left(), rect.top(), rect.right(), rect.bottom())
            elif self.tool == "point":
                self.win.add_point(self.press.x(), self.press.y(), 1)
            self.press = None
            return
        self.setDragMode(QGraphicsView.DragMode.NoDrag)
        super().mouseReleaseEvent(e)

    def contextMenuEvent(self, e):
        e.accept()

    def wheelEvent(self, e):
        f = 1.15 if e.angleDelta().y() > 0 else 1 / 1.15
        self.fit_mode = False
        self.scale(f, f)

    def mouseDoubleClickEvent(self, e):
        if e.button() == Qt.MouseButton.MiddleButton:
            self.fit()


class Picker(QMainWindow):
    def __init__(self, job: Path, frame: int, daemon: str, model: str, node: str = "Hint",
                 helper_python: str | None = None):
        super().__init__()
        self.job, self.frame, self.daemon, self.model = job, frame, daemon, model
        self.node = node
        self.helper_python = helper_python or sys.executable
        self.pv = job / "preview"
        self.objects: list[dict] = []
        self.current = -1
        self.history: list[str] = []
        self.wait_seq = 0
        self.asked_at = 0.0
        self.setWindowTitle(f"CorridorKey {node} – pick what to keep (frame {frame})")
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)

        self.view = View(self)
        side = QVBoxLayout()
        tools = QHBoxLayout()
        self.tool_group = QButtonGroup(self)
        for key, name, tip in (("point", "Point", "Click: keep · right-click: remove · drag: box (P)"),
                               ("box", "Box", "Drag a rectangle: a new object (B)"),
                               ("lasso", "Lasso", "Draw around an area: add it · Alt or right-drag: cut it away (L)")):
            b = QToolButton()
            b.setText(name)
            b.setToolTip(tip)
            b.setCheckable(True)
            b.setChecked(key == "point")
            b.setMinimumWidth(80)
            b.clicked.connect(lambda _=False, k=key: self.set_tool(k))
            self.tool_group.addButton(b)
            tools.addWidget(b)
            setattr(self, f"b_{key}", b)
        side.addLayout(tools)
        self.exact = QCheckBox("Lasso: exact edge")
        self.exact.setChecked(True)
        self.exact.setToolTip("On: the edge is exactly where you drew.\n"
                              "Off: SAM finds the edge inside what you drew (dashed).")
        side.addWidget(self.exact)
        self.help_ = QLabel()
        self.help_.setWordWrap(True)
        side.addWidget(self.help_)
        side.addWidget(QLabel("Objects (clicks go to the selected one):"))
        self.list = QListWidget()
        self.list.currentRowChanged.connect(self.select)
        side.addWidget(self.list, 1)
        row = QHBoxLayout()
        b_new = QPushButton("+ New object")
        b_new.clicked.connect(self.new_object)
        b_del = QPushButton("Remove")
        b_del.clicked.connect(self.remove_object)
        row.addWidget(b_new)
        row.addWidget(b_del)
        side.addLayout(row)
        b_clear = QPushButton("Clear all")
        b_clear.clicked.connect(self.clear_all)
        side.addWidget(b_clear)
        self.status = QLabel("")
        self.status.setWordWrap(True)
        side.addWidget(self.status)
        self.use_btn = QPushButton("✓  Use this selection")
        self.use_btn.setStyleSheet("font-weight:bold; padding:8px")
        self.use_btn.clicked.connect(self.use)
        side.addWidget(self.use_btn)
        b_cancel = QPushButton("Cancel")
        b_cancel.clicked.connect(self.close)
        side.addWidget(b_cancel)

        w = QWidget()
        lay = QHBoxLayout(w)
        sw = QWidget()
        sw.setLayout(side)
        sw.setFixedWidth(300)
        lay.addWidget(self.view, 1)
        lay.addWidget(sw)
        self.setCentralWidget(w)
        self.resize(1400, 820)
        QShortcut(QKeySequence.StandardKey.Undo, self, activated=self.undo)
        QShortcut(QKeySequence("P"), self, activated=lambda: self.set_tool("point"))
        QShortcut(QKeySequence("B"), self, activated=lambda: self.set_tool("box"))
        QShortcut(QKeySequence("L"), self, activated=lambda: self.set_tool("lasso"))
        self.set_tool("point")

        pix = QPixmap(str(self.pv / f"view_{frame:06d}.ppm"))
        if pix.isNull():
            QMessageBox.critical(self, f"CorridorKey {self.node}", "The frame image is missing. "
                                 "Press 'Pick…' in Resolve again.")
            QTimer.singleShot(0, self.close)
            return
        self.view.set_frame(pix)
        old = read_json(self.job / "prompts.json")  # continue an earlier selection on this frame
        if old and int(old.get("frame", -1)) == frame:
            self.objects = [dict({"points": [], "box": None, "regions": []}, **o) for o in old.get("objects", [])]
            self.current = len(self.objects) - 1
            self.refresh_list()
            self.ask()
        self.redraw()
        self.timer = QTimer(self, interval=200, timeout=self.poll)
        self.timer.start()
        self.beat = QTimer(self, interval=2000, timeout=self.heartbeat)
        self.beat.start()
        self.heartbeat()
        self.set_status("Click on what you want to keep, drag a rectangle, or draw a lasso around it.")

    # ---------------------------------------------------------------- editing
    def snapshot(self):
        self.history.append(json.dumps([self.objects, self.current]))
        self.history = self.history[-100:]

    def undo(self):
        if not self.history:
            return
        self.objects, self.current = json.loads(self.history.pop())
        self.changed()

    def add_point(self, x, y, label):
        self.snapshot()
        if self.current < 0 or self.current >= len(self.objects):
            if label == 0 and not self.objects:
                self.history.pop()
                self.set_status("First click on (or drag around) something to keep.")
                return
            if self.current < 0 or not self.objects:
                self.objects.append({"points": [], "box": None, "regions": []})
                self.current = len(self.objects) - 1
        self.objects[self.current]["points"].append([round(x, 1), round(y, 1), label])
        self.changed()

    def add_box(self, x0, y0, x1, y1):
        self.snapshot()
        self.objects.append({"points": [], "box": [round(x0, 1), round(y0, 1), round(x1, 1), round(y1, 1)],
                             "regions": []})
        self.current = len(self.objects) - 1
        self.changed()

    def set_tool(self, key: str):
        self.view.tool = key
        getattr(self, f"b_{key}").setChecked(True)
        self.help_.setText({
            "point": "<b>Click</b> on what to keep · <b>right-click</b> on things to remove · "
                     "<b>drag</b> for a box.",
            "box": "<b>Drag</b> a rectangle around something: each rectangle is its own object.",
            "lasso": "<b>Draw</b> around an area to add it · <b>Alt+draw</b> or <b>right-drag</b> to cut "
                     "it away. Goes to the selected object."}[key]
            + "<br>Wheel: zoom · middle-drag: pan · Ctrl+Z: undo")

    def add_region(self, poly, op):
        self.snapshot()
        if self.current < 0 or self.current >= len(self.objects):
            if op == 0 and not self.objects:
                self.history.pop()
                self.set_status("First add something to keep, then cut parts away.")
                return
            self.objects.append({"points": [], "box": None, "regions": []})
            self.current = len(self.objects) - 1
        self.objects[self.current].setdefault("regions", []).append(
            {"poly": poly, "op": int(op), "exact": self.exact.isChecked()})
        self.changed()

    def new_object(self):
        self.snapshot()
        self.objects.append({"points": [], "box": None, "regions": []})
        self.current = len(self.objects) - 1
        self.changed(ask=False)
        self.set_status(f"Object {self.current + 1}: click on it or drag a rectangle around it.")

    def remove_object(self):
        if 0 <= self.current < len(self.objects):
            self.snapshot()
            del self.objects[self.current]
            self.current = min(self.current, len(self.objects) - 1)
            self.changed()

    def clear_all(self):
        self.snapshot()
        self.objects, self.current = [], -1
        self.changed()

    def select(self, row):
        if row != self.current and 0 <= row < len(self.objects):
            self.current = row
            self.redraw()

    def changed(self, ask=True):
        # drop empty objects except the selected one
        keep = [i for i, o in enumerate(self.objects)
                if o["points"] or o["box"] or o.get("regions") or i == self.current]
        cur = self.objects[self.current] if 0 <= self.current < len(self.objects) else None
        self.objects = [self.objects[i] for i in keep]
        self.current = self.objects.index(cur) if cur in self.objects else len(self.objects) - 1
        self.refresh_list()
        self.redraw()
        if ask:
            self.ask()

    def refresh_list(self):
        self.list.blockSignals(True)
        self.list.clear()
        for i, ob in enumerate(self.objects):
            keep = sum(1 for p in ob["points"] if p[2])
            rem = sum(1 for p in ob["points"] if not p[2])
            add = sum(1 for r in ob.get("regions", []) if r["op"])
            cut = sum(1 for r in ob.get("regions", []) if not r["op"])
            parts = (["rectangle"] if ob["box"] else []) + ([f"{keep} keep"] if keep else []) + \
                    ([f"{rem} remove"] if rem else []) + ([f"{add} lasso"] if add else []) + \
                    ([f"{cut} cut"] if cut else [])
            it = QListWidgetItem(f"Object {i + 1}: " + (", ".join(parts) or "empty"))
            it.setForeground(QColor(COLORS[i % len(COLORS)]))
            self.list.addItem(it)
        self.list.setCurrentRow(self.current)
        self.list.blockSignals(False)

    def redraw(self):
        self.view.draw_marks(self.objects, self.current)

    def usable(self):
        return [o for o in self.objects
                if o["box"] or any(p[2] for p in o["points"]) or any(r["op"] for r in o.get("regions", []))]

    # ---------------------------------------------------------------- SAM preview
    def ask(self):
        if not self.usable():
            self.view.set_mask(None)
            self.set_status("Nothing to keep yet: click on something, drag a rectangle or draw a lasso.")
            return
        self.ensure_daemon()
        self.wait_seq = int(time.time() * 1000)
        self.asked_at = time.time()
        req = {"seq": self.wait_seq, "frame": self.frame, "objects": self.usable(), "model": self.model}
        self.pv.mkdir(parents=True, exist_ok=True)
        atomic_write(self.pv / "request.json", json.dumps(req))
        self.set_status("Working out the mask…")

    def poll(self):
        if not self.wait_seq:
            return
        done = read_json(self.pv / "done.json") or {}
        if done.get("seq", 0) < self.wait_seq:
            secs = time.time() - self.asked_at
            if secs > 4:
                self.set_status(f"Working out the mask… {secs:.0f} s"
                                + ("  (first time: SAM is loading)" if secs > 8 else ""))
            return
        self.wait_seq = 0
        if done.get("error"):
            self.view.set_mask(None)
            self.set_status("✗ " + done["error"] + "\nDetails: " + str(self.job / "engine.log"))
            return
        img = QImage(str(self.pv / "mask_view.png"))
        self.view.set_mask(img)
        self.set_status("Red = kept. Add points or rectangles to fix it, then press "
                        "'Use this selection'.")

    def ensure_daemon(self):
        root = self.job.parent

        def age(p):
            try:
                return time.time() - p.stat().st_mtime
            except OSError:
                return 1e9
        if age(root / "daemon.alive") < 10 or age(root / "daemon.starting") < 60:
            return
        atomic_write(root / "daemon.starting", str(time.time()))
        kw = {"start_new_session": True} if os.name != "nt" else {"creationflags": 0x00000008}
        subprocess.Popen([self.helper_python, self.daemon, str(root), self.model], cwd=str(root),
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, **kw)

    def heartbeat(self):
        try:
            atomic_write(self.job / "picker.open", f"{os.getpid()} {time.time():.0f}\n")
        except OSError:
            pass

    # ---------------------------------------------------------------- finish
    def use(self):
        objs = self.usable()
        if not objs:
            QMessageBox.information(self, f"CorridorKey {self.node}",
                                    "Click on something, drag a rectangle or draw a lasso around it first.")
            return
        atomic_write(self.job / "prompts.json",
                     json.dumps({"frame": self.frame, "objects": objs, "model": self.model}))
        self.close()

    def set_status(self, text):
        self.status.setText(text)

    def closeEvent(self, e):
        try:
            (self.job / "picker.open").unlink()
        except OSError:
            pass
        super().closeEvent(e)


def main():
    job, frame, daemon, model = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
    node = sys.argv[5] if len(sys.argv) > 5 else "Hint"
    helper = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6] else None
    sys.stderr = open(job / "picker.log", "a", buffering=1)
    sys.stdout = sys.stderr
    app = QApplication(sys.argv[:1])
    app.setApplicationName(f"CorridorKey {node}")
    app.setStyle("Fusion")
    from PySide6.QtGui import QPalette
    pal = QPalette()
    for role, col in ((QPalette.ColorRole.Window, "#28282b"), (QPalette.ColorRole.WindowText, "#dddddd"),
                      (QPalette.ColorRole.Base, "#1e1e20"), (QPalette.ColorRole.AlternateBase, "#2c2c2f"),
                      (QPalette.ColorRole.Text, "#dddddd"), (QPalette.ColorRole.Button, "#353538"),
                      (QPalette.ColorRole.ButtonText, "#dddddd"), (QPalette.ColorRole.Highlight, "#3d6fb4"),
                      (QPalette.ColorRole.HighlightedText, "#ffffff")):
        pal.setColor(role, QColor(col))
    app.setPalette(pal)
    w = Picker(job, frame, daemon, model, node, helper)
    w.show()
    w.raise_()
    w.activateWindow()
    if os.environ.get("CK2_PICKER_SCRIPT"):  # tests drive the window from a script
        exec(Path(os.environ["CK2_PICKER_SCRIPT"]).read_text(), {"w": w, "app": app, "QTimer": QTimer})
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
