sh build.sh


echo "Exporting build to dist/backend..."
export_dist_folder_path=../dist/backend

# Ensure the export_dist_folder_path exists, or create it
if [ ! -d "$export_dist_folder_path" ]; then
  mkdir -p "$export_dist_folder_path"
  echo "Created directory: $export_dist_folder_path"
fi



cp -R build/lib/* $export_dist_folder_path/
cp python_bidirectional_ipc_script.py $export_dist_folder_path/
echo "DONE!"

