package dev.bedaim.bridge;
interface IBridgeUserService {
 void destroy() = 16777114;
 String[] listFiles(String path) = 1;
 byte[] readFile(String path,long offset,int length) = 2;
 String writeFile(String path,in byte[] content,long offset,boolean truncate) = 3;
 String makeDirectory(String path) = 4;
 String deletePath(String path,boolean recursive) = 5;
 String copyPath(String source,String destination,boolean overwrite) = 6;
 String movePath(String source,String destination,boolean overwrite) = 7;
}
