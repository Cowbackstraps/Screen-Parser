const elements = {
  analyzeButton: document.querySelector("#analyzeButton"),
  demoViewButton: document.querySelector("#demoViewButton"),
  architectureViewButton: document.querySelector("#architectureViewButton"),
  backToDemoButton: document.querySelector("#backToDemoButton"),
  demoView: document.querySelector("#demoView"),
  architectureView: document.querySelector("#architectureView"),
  archSnapshotStat: document.querySelector("#archSnapshotStat"),
  deviceStatus: document.querySelector("#deviceStatus"),
  screenSize: document.querySelector("#screenSize"),
  resetZoom: document.querySelector("#resetZoom"),
  screenStage: document.querySelector("#screenStage"),
  emptyState: document.querySelector("#emptyState"),
  imageWrap: document.querySelector("#imageWrap"),
  screenImage: document.querySelector("#screenImage"),
  overlay: document.querySelector("#overlay"),
  pageSummary: document.querySelector("#pageSummary"),
  pageTitle: document.querySelector("#pageTitle"),
  pageDescription: document.querySelector("#pageDescription"),
  nodeCount: document.querySelector("#nodeCount"),
  nodeList: document.querySelector("#nodeList"),
  nodeDetail: document.querySelector("#nodeDetail"),
  message: document.querySelector("#message"),
};

const state = { deviceId: null, analysis: null, selectedId: null, zoomTarget: null };

const typeLabels = {
  TEXT: "文字", PICTOGRAM: "图标", IMAGE: "图片", BUTTON: "按钮",
  TEXT_FIELD: "输入框", LIST_ITEM: "列表项", SWITCH: "开关", TAB: "标签",
  NAVIGATION_BAR: "导航栏", TOOLBAR: "工具栏", DIALOG: "弹窗",
  GROUP: "分组", OTHER: "其他",
};

function showView(view) {
  const architecture = view === "architecture";
  elements.demoView.hidden = architecture;
  elements.architectureView.hidden = !architecture;
  elements.demoViewButton.setAttribute("aria-pressed", String(!architecture));
  elements.architectureViewButton.setAttribute("aria-pressed", String(architecture));
  if (!architecture) requestAnimationFrame(syncImageGeometry);
}

function syncImageGeometry() {
  const analysis = state.analysis;
  if (!analysis || elements.imageWrap.hidden) return;

  const stageStyle = getComputedStyle(elements.screenStage);
  const horizontalPadding =
    parseFloat(stageStyle.paddingLeft) + parseFloat(stageStyle.paddingRight);
  const verticalPadding =
    parseFloat(stageStyle.paddingTop) + parseFloat(stageStyle.paddingBottom);
  const availableWidth = Math.max(1, elements.screenStage.clientWidth - horizontalPadding);
  const availableHeight = Math.max(1, elements.screenStage.clientHeight - verticalPadding);
  const scale = Math.min(
    availableWidth / analysis.screen_width,
    availableHeight / analysis.screen_height,
  );

  elements.imageWrap.style.width = `${analysis.screen_width * scale}px`;
  elements.imageWrap.style.height = `${analysis.screen_height * scale}px`;
  applyZoom();
}

function applyZoom() {
  const target = state.zoomTarget;
  elements.resetZoom.hidden = !target;
  elements.screenStage.classList.toggle("is-zoomed", Boolean(target));
  if (!target) {
    elements.imageWrap.style.transform = "";
    return;
  }
  const width = elements.imageWrap.clientWidth;
  const height = elements.imageWrap.clientHeight;
  const stage = elements.screenStage;
  const zoom = Math.min(4.5, Math.max(2.4, stage.clientWidth * 1.05 / width));
  const maxX = Math.max(0, (width * zoom - stage.clientWidth) / 2);
  const maxY = Math.max(0, (height * zoom - stage.clientHeight) / 2);
  const translateX = Math.max(-maxX, Math.min(maxX, (0.5 - target.x) * width * zoom));
  const translateY = Math.max(-maxY, Math.min(maxY, (0.5 - target.y) * height * zoom));
  elements.imageWrap.style.transform = `translate(${translateX}px, ${translateY}px) scale(${zoom})`;
}

function zoomToPoint(x, y) {
  state.zoomTarget = { x: Math.max(0, Math.min(1, x)), y: Math.max(0, Math.min(1, y)) };
  applyZoom();
}

function resetZoom() {
  state.zoomTarget = null;
  applyZoom();
}

async function loadStatus() {
  try {
    const response = await fetch("/api/status");
    const data = await response.json();
    state.deviceId = data.default_device;
    elements.deviceStatus.textContent = state.deviceId ? "HDC已就绪" : "未检测到设备";
    elements.deviceStatus.className = `status ${state.deviceId ? "ready" : "error"}`;
    elements.analyzeButton.disabled = !state.deviceId;
  } catch (error) {
    showError(`状态读取失败：${error.message}`);
    elements.analyzeButton.disabled = true;
  }
}

async function analyzeScreen() {
  showView("demo");
  state.analysis = null;
  state.selectedId = null;
  resetZoom();
  setLoading(true);
  try {
    const response = await fetch("/api/analyze", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device_id: state.deviceId }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "解析请求失败");
    state.analysis = data;
    state.deviceId = data.device_id;
    renderAnalysis();
    return data;
  } catch (error) {
    showError(error.message);
    throw error;
  } finally {
    setLoading(false);
  }
}

function setLoading(loading) {
  elements.analyzeButton.disabled = loading;
  elements.analyzeButton.textContent = loading ? "解析中…" : "开始解析";
  elements.screenStage.classList.toggle("is-loading", loading);
  if (loading && !state.analysis) {
    if (state.deviceId) {
      elements.deviceStatus.textContent = "HDC已就绪";
      elements.deviceStatus.className = "status ready";
    }
    elements.imageWrap.hidden = true;
    elements.pageSummary.hidden = true;
    elements.nodeDetail.hidden = true;
    elements.overlay.replaceChildren();
    elements.nodeList.replaceChildren();
    elements.nodeCount.textContent = "0";
    elements.screenSize.textContent = "—";
    elements.emptyState.hidden = false;
    elements.emptyState.querySelector("strong").textContent = "正在解析";
    elements.message.hidden = false;
    elements.message.className = "message";
    elements.message.textContent = "解析中，请稍候…";
  }
}

function renderAnalysis() {
  const analysis = state.analysis;
  elements.archSnapshotStat.textContent = `${analysis.nodes.length} 个节点 · ${analysis.edges?.length || 0} 条关系`;
  elements.emptyState.hidden = true;
  elements.imageWrap.hidden = false;
  elements.screenImage.src = analysis.screenshot;
  syncImageGeometry();
  elements.screenSize.textContent = `${analysis.screen_width} × ${analysis.screen_height}`;
  elements.pageTitle.textContent = analysis.page.title || analysis.current_app;
  elements.pageDescription.textContent = analysis.page.summary || "暂无页面摘要";
  elements.pageSummary.hidden = false;
  elements.nodeCount.textContent = `${analysis.nodes.length} 个节点`;
  elements.nodeDetail.hidden = true;
  elements.message.hidden = analysis.nodes.length > 0 && !analysis.ocr_error;
  elements.message.className = `message${analysis.ocr_error ? " warning" : ""}`;
  elements.message.textContent = analysis.ocr_error
    ? `PaddleOCR 未完成：${analysis.ocr_error}。已显示视觉模型结果。`
    : "没有识别到有效节点。";
  renderNodes();
  elements.nodeList.scrollTop = 0;
}

function renderNodes() {
  const nodes = state.analysis?.nodes || [];
  elements.overlay.replaceChildren();
  elements.nodeList.replaceChildren();

  nodes.forEach((node, index) => {
    const bounds = node.bounds_norm;
    const label = node.text || node.description || "未命名节点";
    const isOcr = node.sources.includes("ocr");
    const box = document.createElement("button");
    box.type = "button";
    box.className = `node-box ${isOcr ? "ocr" : ""}`;
    box.dataset.nodeId = node.id;
    box.setAttribute("aria-label", `${index + 1}. ${label}`);
    box.style.left = `${bounds.x1 / 10}%`;
    box.style.top = `${bounds.y1 / 10}%`;
    box.style.width = `${(bounds.x2 - bounds.x1) / 10}%`;
    box.style.height = `${(bounds.y2 - bounds.y1) / 10}%`;
    box.innerHTML = `<span class="node-index">${index + 1}</span>`;
    box.addEventListener("click", () => selectNode(node.id));
    elements.overlay.append(box);

    const item = document.createElement("li");
    item.className = `node-row ${isOcr ? "ocr" : ""}`;
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.nodeId = node.id;
    button.innerHTML = `
      <span class="index">${String(index + 1).padStart(2, "0")}</span>
      <span class="node-copy">
        <span class="node-label"></span>
        <span class="node-role"></span>
      </span>
      ${Number.isFinite(node.confidence_calibrated) ? `<span class="confidence">${Math.round(node.confidence_calibrated * 100)}%</span>` : ""}`;
    button.querySelector(".node-label").textContent = label;
    button.querySelector(".node-role").textContent = `${typeLabels[node.type] || node.type}${node.interactive_inferred ? " · 可交互" : ""}`;
    button.addEventListener("click", () => selectNode(node.id));
    item.append(button);
    elements.nodeList.append(item);
  });
}

function selectNode(nodeId) {
  state.selectedId = nodeId;
  document.querySelectorAll("[data-node-id]").forEach((item) => {
    item.classList.toggle("selected", item.dataset.nodeId === nodeId);
  });
  elements.nodeList.querySelector(`button[data-node-id="${CSS.escape(nodeId)}"]`)?.scrollIntoView({ block: "nearest" });
  const node = state.analysis.nodes.find((item) => item.id === nodeId);
  if (!node) return;
  const bounds = node.bounds_norm;
  zoomToPoint((bounds.x1 + bounds.x2) / 2000, (bounds.y1 + bounds.y2) / 2000);
  elements.nodeDetail.hidden = false;
  elements.nodeDetail.replaceChildren();
  const title = document.createElement("strong");
  title.textContent = node.text || node.description || "未命名节点";
  const detail = document.createElement("pre");
  detail.className = "node-json";
  detail.textContent = JSON.stringify(node, null, 2);
  elements.nodeDetail.append(title, detail);
}

function showError(message) {
  elements.emptyState.querySelector("strong").textContent = "解析失败";
  elements.emptyState.querySelector("p").textContent = "请重试";
  elements.message.hidden = false;
  elements.message.className = "message error";
  elements.message.textContent = message;
  elements.deviceStatus.textContent = "解析失败";
  elements.deviceStatus.className = "status error";
}

elements.analyzeButton.addEventListener("click", () => {
  analyzeScreen().catch(() => {});
});
elements.demoViewButton.addEventListener("click", () => showView("demo"));
elements.architectureViewButton.addEventListener("click", () => showView("architecture"));
elements.backToDemoButton.addEventListener("click", () => showView("demo"));
elements.resetZoom.addEventListener("click", resetZoom);
elements.screenImage.addEventListener("click", (event) => {
  state.selectedId = null;
  document.querySelectorAll("[data-node-id].selected").forEach((item) => item.classList.remove("selected"));
  elements.nodeDetail.hidden = true;
  const rect = elements.screenImage.getBoundingClientRect();
  zoomToPoint((event.clientX - rect.left) / rect.width, (event.clientY - rect.top) / rect.height);
});
elements.screenImage.addEventListener("keydown", (event) => {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    zoomToPoint(0.5, 0.5);
  }
});
loadStatus();

function registerWebMcpTool() {
  const context = document.modelContext;
  if (!context?.registerTool) return;

  try {
    Promise.resolve(
      context.registerTool({
        name: "analyze_current_harmony_screen",
        title: "解析屏幕",
        description: "在页面中显示屏幕截图和结构化语义节点。",
        inputSchema: { type: "object", properties: {}, additionalProperties: false },
        annotations: { readOnlyHint: true, untrustedContentHint: true },
        async execute() {
          if (!state.deviceId) throw new Error("未检测到 HDC 设备或模拟器");
          const analysis = await analyzeScreen();
          return {
            device_id: analysis.device_id,
            page: analysis.page,
            node_count: analysis.nodes.length,
          };
        },
      }),
    ).catch(() => {});
  } catch (_) {
    // WebMCP is optional and unsupported browsers keep the normal button flow.
  }
}

registerWebMcpTool();

if ("ResizeObserver" in window) {
  new ResizeObserver(syncImageGeometry).observe(elements.screenStage);
} else {
  window.addEventListener("resize", syncImageGeometry);
}
