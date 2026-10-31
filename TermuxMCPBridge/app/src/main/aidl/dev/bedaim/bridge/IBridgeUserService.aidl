package dev.bedaim.bridge;

interface IBridgeUserService {
    void destroy() = 16777114;
    String[] listFiles(String relativePath);
    byte[] readFile(String relativePath, int maxBytes);
    String writeFile(String relativePath, in byte[] content, boolean overwrite);
}
