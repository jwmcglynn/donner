#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <vector>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererSkia.h"

using namespace donner::base;
using namespace donner::base::parser;
using namespace donner::svg;
using namespace donner::svg::parser;

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "Glfw Error " << error << ": " << description << std::endl;
}

void loadSVG(const char* filename, SVGDocument& document) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Could not open file " << filename << std::endl;
        std::abort();
    }

    XMLParser::InputBuffer fileData;
    fileData.loadFromStream(file);

    std::vector<ParseError> warnings;
    ParseResult<SVGDocument> maybeDocument = XMLParser::ParseSVG(fileData, &warnings);

    if (maybeDocument.hasError()) {
        const ParseError& e = maybeDocument.error();
        std::cerr << "Parse Error: " << e << std::endl;
        std::abort();
    }

    std::cout << "Parsed successfully." << std::endl;

    if (!warnings.empty()) {
        std::cout << "Warnings:" << std::endl;
        for (ParseError& w : warnings) {
            std::cout << "  " << w << std::endl;
        }
    }

    document = std::move(maybeDocument.result());
}

void renderSVG(SVGDocument& document, RendererSkia& renderer) {
    renderer.draw(document);
    const SkBitmap& bitmap = renderer.bitmap();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.width(), bitmap.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.getPixels());
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: svg_viewer <filename>" << std::endl;
        return 1;
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "SVG Viewer", NULL, NULL);
    if (window == NULL) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize OpenGL loader!" << std::endl;
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    SVGDocument document;
    RendererSkia renderer;
    loadSVG(argv[1], document);
    renderSVG(document, renderer);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("SVG Viewer");
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::Image((void*)(intptr_t)texture, ImVec2((float)renderer.width(), (float)renderer.height()));
        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
