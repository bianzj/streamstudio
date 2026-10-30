from __future__ import annotations

import html
import json
from datetime import date
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate,
    Frame,
    KeepTogether,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)
from reportlab.platypus.tableofcontents import TableOfContents


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "docs"
VERSION = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))["version"]
BUILD_DATE = date.today().isoformat()
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

FONT_REGULAR = Path(r"C:\Windows\Fonts\Deng.ttf")
FONT_BOLD = Path(r"C:\Windows\Fonts\Dengb.ttf")
pdfmetrics.registerFont(TTFont("StreamCN", str(FONT_REGULAR)))
pdfmetrics.registerFont(TTFont("StreamCN-Bold", str(FONT_BOLD)))
pdfmetrics.registerFontFamily(
    "StreamCN",
    normal="StreamCN",
    bold="StreamCN-Bold",
    italic="StreamCN",
    boldItalic="StreamCN-Bold",
)

PAGE_W, PAGE_H = A4
NAVY = colors.HexColor("#10273D")
BLUE = colors.HexColor("#2674D9")
CYAN = colors.HexColor("#3BB4C1")
TEXT = colors.HexColor("#23384B")
MUTED = colors.HexColor("#687D90")
LINE = colors.HexColor("#D9E4EC")
PALE = colors.HexColor("#F2F7FA")
PALE_BLUE = colors.HexColor("#EAF2FD")


def latex_plain(value: str) -> str:
    replacements = {
        r"\rightarrow": "→",
        r"\times": "×",
        r"\cdot": "·",
        r"\cdots": "⋯",
        r"\int": "∫",
        r"\leq": "≤",
        r"\geq": "≥",
        r"\approx": "≈",
        r"\alpha": "α",
        r"\beta": "β",
        r"\gamma": "γ",
        r"\Gamma": "Γ",
        r"\Delta": "Δ",
        r"\delta": "δ",
        r"\epsilon": "ε",
        r"\varepsilon": "ε",
        r"\eta": "η",
        r"\kappa": "κ",
        r"\lambda": "λ",
        r"\mu": "μ",
        r"\nu": "ν",
        r"\rho": "ρ",
        r"\sigma": "σ",
        r"\tau": "τ",
        r"\theta": "θ",
        r"\omega": "ω",
        r"\Omega": "Ω",
        r"\phi": "φ",
        r"\pi": "π",
        r"\partial": "∂",
        r"\nabla": "∇",
        r"\uparrow": "↑",
        r"\downarrow": "↓",
        r"\ell": "ℓ",
        r"\sum": "Σ",
        r"\prod": "Π",
        r"\infty": "∞",
    }
    value = value.replace("$", "")
    value = value.replace(r"\dfrac", r"\frac").replace(r"\tfrac", r"\frac")
    value = re.sub(r"\\(?:mathrm|text|boldsymbol|mathbf|operatorname|mathcal)\{([^{}]*)\}", r"\1", value)
    value = re.sub(r"\\(?:boldsymbol|mathbf)\s*(?=\\|[A-Za-z])", "", value)
    value = re.sub(r"\\overline\{([^{}]*)\}", r"mean(\1)", value)
    value = re.sub(r"\\hat\{([^{}]*)\}", r"\1_hat", value)
    value = re.sub(r"\\(?:left|right)(?![A-Za-z])", "", value)
    value = value.replace(r"\begin{aligned}", "").replace(r"\end{aligned}", "")
    value = re.sub(r"\\[A-Za-z]+", lambda match: replacements.get(match.group(0), match.group(0)), value)
    for _ in range(4):
        updated = re.sub(r"_\{([^{}]+)\}", r"_(\1)", value)
        updated = re.sub(r"\^\{([^{}]+)\}", r"^(\1)", updated)
        updated = re.sub(r"\\frac\{([^{}]+)\}\s*\{([^{}]+)\}", r"(\1)/(\2)", updated)
        updated = re.sub(r"\\sqrt\{([^{}]+)\}", r"sqrt(\1)", updated)
        if updated == value:
            break
        value = updated
    value = re.sub(r"\\tag\{([^{}]+)\}", r"[\1]", value)
    value = value.replace(r"\exp", "exp").replace(r"\ln", "ln")
    value = value.replace(r"\sin", "sin").replace(r"\cos", "cos")
    value = value.replace(r"\max", "max").replace(r"\min", "min")
    value = value.replace(r"\qquad", "    ").replace(r"\quad", "  ")
    value = value.replace(r"\,", " ").replace(r"\;", " ").replace(r"\!", "").replace("\\ ", " ")
    value = value.replace("&", "").replace(r"\\", "")
    return value


def inline_markup(value: str) -> str:
    value = latex_plain(value)
    value = html.escape(value.strip(), quote=False)
    value = re.sub(
        r"\[([^\]]+)\]\((https?://[^)]+)\)",
        r'<link href="\2" color="#2674D9">\1</link>',
        value,
    )
    # Relative links (other markdown files) cannot be opened from the PDF;
    # keep the label and drop the target.
    value = re.sub(r"\[([^\]]+)\]\((?!https?://)[^)]+\)", r"\1", value)
    # Inline code is stashed before emphasis so that asterisks inside two
    # different code spans cannot be paired into one <i>...</i>.
    codes: list[str] = []

    def stash(match: re.Match) -> str:
        codes.append(match.group(1))
        return f"\x00{len(codes) - 1}\x00"

    value = re.sub(r"`([^`]+)`", stash, value)
    value = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", value)
    value = re.sub(r"\*([^*]+)\*", r"<i>\1</i>", value)
    value = re.sub(
        r"\x00(\d+)\x00",
        lambda match: f'<font color="#145A78">{codes[int(match.group(1))]}</font>',
        value,
    )
    return value


def split_table_row(line: str) -> list[str]:
    return [part.strip() for part in line.strip().strip("|").split("|")]


class ManualDocTemplate(BaseDocTemplate):
    def __init__(self, filename: str, manual_title: str):
        super().__init__(
            filename,
            pagesize=A4,
            leftMargin=18 * mm,
            rightMargin=18 * mm,
            topMargin=18 * mm,
            bottomMargin=17 * mm,
            title=manual_title,
            author="Stream Studio Team",
            subject=f"Stream Studio {VERSION} 中文技术文档",
        )
        self.manual_title = manual_title
        frame = Frame(
            self.leftMargin,
            self.bottomMargin,
            self.width,
            self.height,
            leftPadding=0,
            rightPadding=0,
            topPadding=0,
            bottomPadding=0,
        )
        self.addPageTemplates(PageTemplate("content", [frame], onPage=self.draw_page))

    def draw_page(self, canvas, doc):
        if doc.page == 1:
            return
        canvas.saveState()
        canvas.setStrokeColor(LINE)
        canvas.setLineWidth(0.5)
        canvas.line(18 * mm, PAGE_H - 12 * mm, PAGE_W - 18 * mm, PAGE_H - 12 * mm)
        canvas.setFont("StreamCN", 8)
        canvas.setFillColor(MUTED)
        canvas.drawString(18 * mm, PAGE_H - 9 * mm, self.manual_title)
        canvas.drawRightString(PAGE_W - 18 * mm, 9 * mm, f"Stream Studio {VERSION}  ·  {doc.page - 1}")
        canvas.restoreState()

    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph):
            level = getattr(flowable, "toc_level", None)
            if level is not None:
                text = flowable.getPlainText()
                key = f"h-{self.page}-{abs(hash(text))}"
                self.canv.bookmarkPage(key)
                self.canv.addOutlineEntry(text, key, level=level, closed=False)
                self.notify("TOCEntry", (level, text, self.page - 1, key))


def make_styles():
    sample = getSampleStyleSheet()
    styles = {
        "body": ParagraphStyle(
            "BodyCN",
            parent=sample["BodyText"],
            fontName="StreamCN",
            fontSize=9.4,
            leading=15.2,
            textColor=TEXT,
            alignment=TA_JUSTIFY,
            wordWrap="CJK",
            spaceAfter=2.4 * mm,
        ),
        "h1": ParagraphStyle(
            "Heading1CN",
            fontName="StreamCN-Bold",
            fontSize=18,
            leading=24,
            textColor=NAVY,
            spaceBefore=2 * mm,
            spaceAfter=5 * mm,
            keepWithNext=True,
        ),
        "h2": ParagraphStyle(
            "Heading2CN",
            fontName="StreamCN-Bold",
            fontSize=13,
            leading=18,
            textColor=BLUE,
            spaceBefore=4 * mm,
            spaceAfter=2.5 * mm,
            keepWithNext=True,
        ),
        "h3": ParagraphStyle(
            "Heading3CN",
            fontName="StreamCN-Bold",
            fontSize=10.5,
            leading=15,
            textColor=NAVY,
            spaceBefore=3 * mm,
            spaceAfter=1.5 * mm,
            keepWithNext=True,
        ),
        "bullet": ParagraphStyle(
            "BulletCN",
            fontName="StreamCN",
            fontSize=9.2,
            leading=14.5,
            textColor=TEXT,
            leftIndent=5.5 * mm,
            firstLineIndent=-3.4 * mm,
            bulletIndent=1.1 * mm,
            wordWrap="CJK",
            spaceAfter=1.1 * mm,
        ),
        "number": ParagraphStyle(
            "NumberCN",
            fontName="StreamCN",
            fontSize=9.2,
            leading=14.5,
            textColor=TEXT,
            leftIndent=7 * mm,
            firstLineIndent=-5 * mm,
            wordWrap="CJK",
            spaceAfter=1.1 * mm,
        ),
        "code": ParagraphStyle(
            "CodeCN",
            fontName="StreamCN",
            fontSize=7.5,
            leading=11.2,
            textColor=colors.HexColor("#173B50"),
            leftIndent=1.5 * mm,
            rightIndent=1.5 * mm,
            wordWrap="CJK",
        ),
        "table": ParagraphStyle(
            "TableCN",
            fontName="StreamCN",
            fontSize=7.8,
            leading=11.4,
            textColor=TEXT,
            wordWrap="CJK",
        ),
        "table_head": ParagraphStyle(
            "TableHeadCN",
            fontName="StreamCN-Bold",
            fontSize=7.9,
            leading=11.4,
            textColor=colors.white,
            wordWrap="CJK",
        ),
        "toc_title": ParagraphStyle(
            "TocTitleCN",
            fontName="StreamCN-Bold",
            fontSize=20,
            leading=26,
            textColor=NAVY,
            spaceAfter=8 * mm,
        ),
    }
    return styles


def code_box(lines: list[str], styles, math_mode: bool = False) -> Table:
    cleaned = []
    if math_mode:
        # Convert the whole display block at once: \frac numerators and
        # denominators are often split across source lines.
        expression = latex_plain(" ".join(line.strip() for line in lines if line.strip()))
        expression = re.sub(r"\\([A-Za-z]+)", r"\1", expression)
        lines = [re.sub(r"\s+", " ", expression).strip()]
    for line in lines:
        line = line.replace("\t", "    ")
        cleaned.append(html.escape(line, quote=False) if line else "&#160;")
    paragraph = Paragraph("<br/>".join(cleaned), styles["code"])
    table = Table([[paragraph]], colWidths=[174 * mm], hAlign="LEFT")
    table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, -1), PALE),
                ("BOX", (0, 0), (-1, -1), 0.5, LINE),
                ("LEFTPADDING", (0, 0), (-1, -1), 2.5 * mm),
                ("RIGHTPADDING", (0, 0), (-1, -1), 2.5 * mm),
                ("TOPPADDING", (0, 0), (-1, -1), 2 * mm),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 2 * mm),
            ]
        )
    )
    return table


def markdown_table(lines: list[str], styles) -> Table:
    rows = [split_table_row(line) for line in lines]
    if len(rows) >= 2 and all(re.fullmatch(r":?-{3,}:?", cell or "-") for cell in rows[1]):
        rows.pop(1)
    width = 174 * mm
    columns = max(len(row) for row in rows)
    normalized = [row + [""] * (columns - len(row)) for row in rows]
    data = []
    for row_index, row in enumerate(normalized):
        style = styles["table_head"] if row_index == 0 else styles["table"]
        data.append([Paragraph(inline_markup(cell), style) for cell in row])
    table = Table(data, colWidths=[width / columns] * columns, repeatRows=1, hAlign="LEFT")
    table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                ("BACKGROUND", (0, 1), (-1, -1), colors.white),
                ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, PALE]),
                ("GRID", (0, 0), (-1, -1), 0.35, LINE),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 2 * mm),
                ("RIGHTPADDING", (0, 0), (-1, -1), 2 * mm),
                ("TOPPADDING", (0, 0), (-1, -1), 1.7 * mm),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 1.7 * mm),
            ]
        )
    )
    return table


def markdown_story(source: Path, styles):
    lines = source.read_text(encoding="utf-8").splitlines()
    story = []
    paragraph_lines: list[str] = []
    code_lines: list[str] = []
    table_lines: list[str] = []
    in_code = False
    in_math = False
    chapter_seen = False

    def flush_paragraph():
        if paragraph_lines:
            text = " ".join(item.strip() for item in paragraph_lines)
            story.append(Paragraph(inline_markup(text), styles["body"]))
            paragraph_lines.clear()

    def flush_table():
        if table_lines:
            story.append(markdown_table(table_lines.copy(), styles))
            story.append(Spacer(1, 2.5 * mm))
            table_lines.clear()

    for raw in lines[1:]:
        line = raw.rstrip()
        stripped = line.strip()

        if stripped.startswith("```"):
            flush_paragraph()
            flush_table()
            if in_code:
                story.append(code_box(code_lines.copy(), styles))
                story.append(Spacer(1, 2.5 * mm))
                code_lines.clear()
            in_code = not in_code
            continue
        if stripped == "$$":
            flush_paragraph()
            flush_table()
            if in_math:
                story.append(code_box(code_lines.copy(), styles, math_mode=True))
                story.append(Spacer(1, 2.5 * mm))
                code_lines.clear()
            in_math = not in_math
            continue
        if in_code or in_math:
            code_lines.append(line)
            continue

        if stripped.startswith("|") and stripped.endswith("|"):
            flush_paragraph()
            table_lines.append(stripped)
            continue
        flush_table()

        if not stripped:
            flush_paragraph()
            continue
        if stripped.startswith("## "):
            flush_paragraph()
            if chapter_seen:
                story.append(PageBreak())
            chapter_seen = True
            heading = Paragraph(inline_markup(stripped[3:]), styles["h1"])
            heading.toc_level = 0
            story.append(heading)
            continue
        if stripped.startswith("### "):
            flush_paragraph()
            heading = Paragraph(inline_markup(stripped[4:]), styles["h2"])
            heading.toc_level = 1
            story.append(heading)
            continue
        if stripped.startswith("#### "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[5:]), styles["h3"]))
            continue
        bullet = re.match(r"^-\s+(.*)$", stripped)
        if bullet:
            flush_paragraph()
            story.append(Paragraph(inline_markup(bullet.group(1)), styles["bullet"], bulletText="•"))
            continue
        numbered = re.match(r"^(\d+)\.\s+(.*)$", stripped)
        if numbered:
            flush_paragraph()
            story.append(
                Paragraph(
                    inline_markup(numbered.group(2)),
                    styles["number"],
                    bulletText=f"{numbered.group(1)}.",
                )
            )
            continue
        paragraph_lines.append(stripped)

    flush_paragraph()
    flush_table()
    if code_lines:
        story.append(code_box(code_lines, styles))
    return story


def cover(title: str, subtitle: str, styles):
    return [
        Spacer(1, 38 * mm),
        Table(
            [[""]],
            colWidths=[22 * mm],
            rowHeights=[22 * mm],
            style=TableStyle(
                [
                    ("BACKGROUND", (0, 0), (-1, -1), BLUE),
                    ("BOX", (0, 0), (-1, -1), 0, BLUE),
                ]
            ),
        ),
        Spacer(1, 8 * mm),
        Paragraph(
            title,
            ParagraphStyle(
                "CoverTitle",
                fontName="StreamCN-Bold",
                fontSize=30,
                leading=40,
                textColor=NAVY,
                alignment=TA_LEFT,
            ),
        ),
        Spacer(1, 5 * mm),
        Paragraph(
            subtitle,
            ParagraphStyle(
                "CoverSubtitle",
                fontName="StreamCN",
                fontSize=14,
                leading=21,
                textColor=BLUE,
            ),
        ),
        Spacer(1, 18 * mm),
        Table(
            [
                [Paragraph("软件版本", styles["table"]), Paragraph(VERSION, styles["table"])],
                [Paragraph("工程格式", styles["table"]), Paragraph("schemaVersion = 6", styles["table"])],
                [Paragraph("更新日期", styles["table"]), Paragraph(BUILD_DATE, styles["table"])],
            ],
            colWidths=[35 * mm, 70 * mm],
            style=TableStyle(
                [
                    ("BACKGROUND", (0, 0), (-1, -1), PALE_BLUE),
                    ("GRID", (0, 0), (-1, -1), 0.5, LINE),
                    ("LEFTPADDING", (0, 0), (-1, -1), 3 * mm),
                    ("RIGHTPADDING", (0, 0), (-1, -1), 3 * mm),
                    ("TOPPADDING", (0, 0), (-1, -1), 2.2 * mm),
                    ("BOTTOMPADDING", (0, 0), (-1, -1), 2.2 * mm),
                ]
            ),
        ),
        Spacer(1, 35 * mm),
        Paragraph(
            "STREAM STUDIO · THREE-DIMENSIONAL RADIATIVE TRANSFER & ENERGY BALANCE",
            ParagraphStyle(
                "CoverFooter",
                fontName="StreamCN",
                fontSize=8.5,
                leading=12,
                textColor=MUTED,
                alignment=TA_LEFT,
            ),
        ),
        PageBreak(),
    ]


def build_manual(source: Path, output: Path, title: str, subtitle: str):
    styles = make_styles()
    doc = ManualDocTemplate(str(output), title)
    toc = TableOfContents()
    toc.levelStyles = [
        ParagraphStyle(
            "TOC1",
            fontName="StreamCN-Bold",
            fontSize=10,
            leading=17,
            textColor=NAVY,
            leftIndent=0,
            firstLineIndent=0,
            spaceBefore=1.5 * mm,
        ),
        ParagraphStyle(
            "TOC2",
            fontName="StreamCN",
            fontSize=8.5,
            leading=14,
            textColor=TEXT,
            leftIndent=7 * mm,
            firstLineIndent=0,
        ),
    ]
    story = cover(title, subtitle, styles)
    story.extend([Paragraph("目录", styles["toc_title"]), toc, PageBreak()])
    story.extend(markdown_story(source, styles))
    doc.multiBuild(story)


def main():
    build_manual(
        ROOT / "docs" / "user-manual.md",
        OUTPUT_DIR / "Stream_Studio_User_Manual_CN.pdf",
        "Stream Studio 使用手册",
        "三维场景与卫星单景模拟：工程、配置、运行与结果",
    )
    build_manual(
        ROOT / "docs" / "theory-manual.md",
        OUTPUT_DIR / "Stream_Studio_Theory_Manual_CN.pdf",
        "Stream Studio 理论手册",
        "三维辐射传输、能量平衡与多过程耦合基础",
    )
    print(OUTPUT_DIR / "Stream_Studio_User_Manual_CN.pdf")
    print(OUTPUT_DIR / "Stream_Studio_Theory_Manual_CN.pdf")


if __name__ == "__main__":
    main()
