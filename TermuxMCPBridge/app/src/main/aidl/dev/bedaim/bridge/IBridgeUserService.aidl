package dev.bedaim.bridge;
interface IBridgeUserService {
 void destroy() = 16777114;
 String[] listFiles(String path);
 byte[] readFile(String path,long offset,int length);
 String writeFile(String path,in byte[] content,long offset,boolean truncate);
 String makeDirectory(String path);
 String deletePath(String path,boolean recursive);
 String copyPath(String source,String destination,boolean overwrite);
 String movePath(String source,String destination,boolean overwrite);
}